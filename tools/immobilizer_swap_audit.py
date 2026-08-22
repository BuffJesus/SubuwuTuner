#!/usr/bin/env python3
"""Audit owner-authorized OEM-ECU swap module and registration readiness."""
from __future__ import annotations

import argparse
import json
import sys
import tomllib
from pathlib import Path

DISPOSITIONS = {"retained", "replaced", "donor_matched"}
REGISTRATION_STATES = {"not_required", "registered", "unconfirmed"}


def _load_toml(path):
    return tomllib.loads(Path(path).read_text(encoding="utf-8"))


def load_profile(path):
    doc = _load_toml(path)
    profile = doc.get("profile", {})
    if profile.get("schema_version") != 1:
        raise ValueError("unsupported or missing profile schema_version")
    ids = [str(module.get("id", "")) for module in doc.get("module", [])]
    if not ids or any(not module_id for module_id in ids) or len(ids) != len(set(ids)):
        raise ValueError("profile module ids must be present and unique")
    for link in doc.get("link", []):
        if link.get("from") not in ids or link.get("to") not in ids:
            raise ValueError("profile link references unknown module")
    return doc


def load_manifest(path):
    doc = _load_toml(path)
    swap = doc.get("swap", {})
    if swap.get("schema_version") != 1:
        raise ValueError("unsupported or missing swap manifest schema_version")
    if not swap.get("profile"):
        raise ValueError("swap manifest profile must be present")
    modules = doc.get("module", [])
    ids = [str(module.get("id", "")) for module in modules]
    if not ids or any(not module_id for module_id in ids) or len(ids) != len(set(ids)):
        raise ValueError("manifest module ids must be present and unique")
    for module in modules:
        if module.get("disposition") not in DISPOSITIONS:
            raise ValueError(
                f"module {module['id']!r} has invalid disposition {module.get('disposition')!r}"
            )
        if module.get("registration") not in REGISTRATION_STATES:
            raise ValueError(
                f"module {module['id']!r} has invalid registration state "
                f"{module.get('registration')!r}"
            )
        if not isinstance(module.get("present"), bool):
            raise ValueError(f"module {module['id']!r} must declare present = true or false")
    return doc


def audit(doc, present, registered):
    """Compatibility audit for callers that only know presence/registration sets."""
    modules = doc["module"]
    known = {module["id"] for module in modules}
    required = {module["id"] for module in modules if module.get("required_to_start")}
    needs_registration = {
        module["id"] for module in modules if module.get("replacement_requires_registration")
    }
    missing = sorted(required - present)
    unregistered = sorted((present & needs_registration) - registered)
    return {
        "profile": doc["profile"]["id"],
        "registration_class": doc["profile"]["registration_class"],
        "ready": not missing and not unregistered,
        "missing_required_modules": missing,
        "present_registration_unconfirmed": unregistered,
        "unknown_modules": sorted(present - known),
        "observable_effects": doc.get("effect", []),
        "diagnostic_codes": doc.get("dtc", []),
    }


def audit_manifest(profile, manifest):
    profile_id = profile["profile"]["id"]
    manifest_profile = manifest["swap"]["profile"]
    if manifest_profile != profile_id:
        raise ValueError(
            f"manifest profile {manifest_profile!r} does not match loaded profile {profile_id!r}"
        )
    requirements = {module["id"]: module for module in profile["module"]}
    declared = {module["id"]: module for module in manifest["module"]}
    missing = sorted(
        module_id
        for module_id, requirement in requirements.items()
        if requirement.get("required_to_start")
        and (module_id not in declared or not declared[module_id]["present"])
    )
    unregistered = sorted(
        module_id
        for module_id, state in declared.items()
        if state["present"]
        and module_id in requirements
        and requirements[module_id].get("replacement_requires_registration")
        and state["disposition"] in {"replaced", "donor_matched"}
        and state["registration"] != "registered"
    )
    evidence_missing = sorted(
        module_id
        for module_id, state in declared.items()
        if state["present"]
        and state["disposition"] in {"replaced", "donor_matched"}
        and state["registration"] == "registered"
        and not str(state.get("evidence", "")).strip()
    )
    unknown = sorted(set(declared) - set(requirements))
    return {
        "project": manifest["swap"].get("project", ""),
        "profile": profile_id,
        "registration_class": profile["profile"]["registration_class"],
        "ready": not missing and not unregistered and not evidence_missing,
        "missing_required_modules": missing,
        "present_registration_unconfirmed": unregistered,
        "registration_evidence_missing": evidence_missing,
        "unknown_modules": unknown,
        "modules": manifest["module"],
        "observable_effects": profile.get("effect", []),
        "diagnostic_codes": profile.get("dtc", []),
    }


def _print_text(result):
    print(f"Profile: {result['profile']} ({result['registration_class']})")
    if result.get("project"):
        print(f"Project: {result['project']}")
    print(f"Owner-authorized start readiness: {'READY' if result['ready'] else 'BLOCKED'}")
    for key in (
        "missing_required_modules",
        "present_registration_unconfirmed",
        "registration_evidence_missing",
        "unknown_modules",
    ):
        if result.get(key):
            print(f"{key}: {', '.join(result[key])}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--present", action="append", default=[])
    parser.add_argument("--registered", action="append", default=[])
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    if args.manifest and (args.present or args.registered):
        parser.error("--manifest cannot be combined with --present or --registered")
    try:
        profile = load_profile(args.profile)
        result = (
            audit_manifest(profile, load_manifest(args.manifest))
            if args.manifest
            else audit(profile, set(args.present), set(args.registered))
        )
    except (OSError, ValueError, tomllib.TOMLDecodeError) as exc:
        print(f"swap-audit: {exc}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        _print_text(result)
    return 0 if result["ready"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
