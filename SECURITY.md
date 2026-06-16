# Security Policy

## Supported versions

SubuwuTuner is pre-1.0. Only the latest commit on `main` is supported for security review. Once v1.0 ships, the latest release line will be supported until the next minor release lands.

## Scope

In scope:

- **Code-execution and memory-safety bugs** in the C++ engine (`src/core/`, `src/rom/`, `src/defs/`, `src/edit/`, `src/project/`, `src/transport/`, `src/ecu/`, `src/log/`, `src/can/`, `src/dbc/`, `src/discover/`, `src/flash/`, `src/autotune/`, `src/policy/`, `src/feature/`, `src/feature_codegen/`, `src/feature_patch/`, `src/devices/ets/`, `src/library/`, `src/audit/`, `src/profile/`, `src/diff/`, `src/cli/`, `src/ui/`).
- **Input-handling bugs** in any parser: TOML packs, DBC, ROM formats (`.bin`, `.hex`, `.srec`, `.mot`), `.stune`, `.stcompare` (when shipped), log CSV, `.ptm` (AP tune envelope — XTEA / base64 / AES / bzip2 chain), `.img` (AP OTA envelope — Blowfish), `.stmod` (custom-feature graph).
- **Cryptographic correctness** in `st::ecu::subaru` SecurityAccess implementations, the `.ptm` / `.img` cipher chains under `src/devices/ets/`, and any signature-verification path.
- **Privacy regressions** — anything that exfiltrates ROM bytes, VIN, ECU-ID, paths, or user identifiers off-machine without explicit user opt-in.
- **Safety-gate bypass** — paths that allow the flash orchestrator to write without the documented pre-flight validators, without a verified backup, or without a known checksum strategy.
- **Build/CI supply-chain issues** — anything that could let a CI lane execute attacker-controlled code on a maintainer's machine.

Out of scope:

- **Reports requesting help bypassing OEM ECU protections** (e.g. "how do I extract Subaru's SecurityAccess key for ECU X"). The project does not ship that capability and does not accept those as security reports.
- **Reports asking for tuning support** for unsupported ECUs. Use the public issue tracker.
- **Emissions or regulatory-compliance disagreements.** `docs/06-legal-ethics.md` describes the project's stance; complaints about the stance are not security issues.
- **Issues in third-party libraries** (Catch2, GLFW, ImGui, ImPlot, tomlplusplus, tl::expected, nativefiledialog-extended, libusb-1.0, tiny-AES-c, bzip2). Report upstream; we'll patch dependents as upstream releases land.
- **Issues in the user's own ECU firmware or hardware**.

## Reporting a vulnerability

Please **do not** open a public GitHub issue for a confirmed or suspected security vulnerability.

Email: `noreply+security@github.com` *(placeholder — replace with a monitored address before public release)*

Include:

- A clear description of the issue.
- Reproduction steps and a minimal failing input where possible.
- Affected version (commit SHA is fine).
- Your assessment of impact.
- Whether you intend to disclose publicly, and if so, on what timeline.

You will receive an acknowledgement within **7 days**. A more detailed response, including any timeline for a fix, will follow within **30 days**. We do not currently operate a bounty program.

Please give us a **90-day disclosure window** by default. If the issue is being actively exploited or has a public proof-of-concept, that window may compress; let us know in your initial report.

## Coordinated disclosure

Once a fix is available, we'll:

1. Land the fix on `main`.
2. Cut a release that includes the fix.
3. Publish a GitHub Security Advisory describing the issue, the fix, and credit (if you wish to be credited).
4. Update `CHANGELOG.md` under `### Security`.

If you'd prefer to be uncredited, say so in your initial email.

## Threat model summary

SubuwuTuner connects to a real, expensive piece of hardware (the ECU) and can persist arbitrary bytes to it. Our threat model emphasizes:

- **Local code execution is the most damaging outcome.** A malicious TOML pack, ROM, or log that triggers RCE could chain into an unsafe flash. Parsers are first-priority for safety-critical review.
- **The flash orchestrator is the last line of defense.** Any bypass of the documented preflight pipeline (`st::policy::FlashPreflight`) or the backup gate (`st::flash::BackupStore`) is treated as critical.
- **The user is generally trusted, but their inputs are not.** A user opening a community pack is a sandbox-crossing event from our perspective; the pack is untrusted.

## Things that are not security bugs

- **Refusal to flash for safety reasons.** That's the design.
- **Refusal to write a calibration that disables knock control or rev limit without explicit acknowledgment.** That's the design.
- **A SecurityAccess implementation not working on an ECU we don't claim to support.** Use the public issue tracker.
- **A "this disclaimer is too scary" complaint.** Use the public issue tracker.
