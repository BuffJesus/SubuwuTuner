import tempfile
import unittest
from pathlib import Path
from tools.immobilizer_swap_audit import audit, audit_manifest, load_manifest, load_profile
ROOT=Path(__file__).resolve().parents[2]

class ImmobilizerSwapAuditTests(unittest.TestCase):
    def test_missing_modules_blocks(self):
        doc=load_profile(ROOT/"fixtures/security_domains/2017-wrx-keyed.toml")
        self.assertIn("body_integrated_unit",audit(doc,{"ecm"},{"ecm"})["missing_required_modules"])
    def test_complete_registered_set_ready(self):
        doc=load_profile(ROOT/"fixtures/security_domains/2017-wrx-keyed.toml")
        required={m["id"] for m in doc["module"] if m["required_to_start"]}
        self.assertTrue(audit(doc,required,required)["ready"])
    def test_unregistered_replacement_blocks(self):
        doc=load_profile(ROOT/"fixtures/security_domains/2017-wrx-pushbutton.toml")
        present={m["id"] for m in doc["module"] if m["required_to_start"]}
        self.assertIn("ecm",audit(doc,present,set())["present_registration_unconfirmed"])

    def test_example_manifest_is_ready(self):
        profile=load_profile(ROOT/"fixtures/security_domains/2017-wrx-keyed.toml")
        manifest=load_manifest(ROOT/"fixtures/security_domains/examples/2017-wrx-keyed-donor-swap.toml")
        result=audit_manifest(profile,manifest)
        self.assertTrue(result["ready"])
        self.assertEqual(result["modules"][0]["disposition"],"donor_matched")

    def test_donor_matched_without_registration_blocks(self):
        profile=load_profile(ROOT/"fixtures/security_domains/2017-wrx-keyed.toml")
        manifest=load_manifest(ROOT/"fixtures/security_domains/examples/2017-wrx-keyed-donor-swap.toml")
        manifest["module"][0]["registration"]="unconfirmed"
        result=audit_manifest(profile,manifest)
        self.assertFalse(result["ready"])
        self.assertEqual(result["present_registration_unconfirmed"],["ecm"])

    def test_manifest_profile_must_match(self):
        profile=load_profile(ROOT/"fixtures/security_domains/2017-wrx-pushbutton.toml")
        manifest=load_manifest(ROOT/"fixtures/security_domains/examples/2017-wrx-keyed-donor-swap.toml")
        with self.assertRaisesRegex(ValueError,"does not match"):
            audit_manifest(profile,manifest)

    def test_registered_replacement_requires_evidence(self):
        profile=load_profile(ROOT/"fixtures/security_domains/2017-wrx-keyed.toml")
        manifest=load_manifest(ROOT/"fixtures/security_domains/examples/2017-wrx-keyed-donor-swap.toml")
        del manifest["module"][0]["evidence"]
        result=audit_manifest(profile,manifest)
        self.assertFalse(result["ready"])
        self.assertEqual(result["registration_evidence_missing"],["ecm"])

    def test_manifest_rejects_invalid_disposition(self):
        text='''[swap]\nschema_version=1\nprofile="2017-wrx-keyed"\n[[module]]\nid="ecm"\ndisposition="unknown"\npresent=true\nregistration="unconfirmed"\n'''
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/"swap.toml"
            path.write_text(text,encoding="utf-8")
            with self.assertRaisesRegex(ValueError,"invalid disposition"):
                load_manifest(path)
if __name__=="__main__": unittest.main()
