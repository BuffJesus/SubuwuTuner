import tempfile
import unittest
from pathlib import Path

from tools.rom_function_parity import compare
from tools.rom_name_projection import build_projection


class RomNameProjectionTests(unittest.TestCase):
    def test_only_named_to_unnamed_match_is_eligible(self):
        with tempfile.TemporaryDirectory() as raw:
            root=Path(raw); left=root/"left.bin"; right=root/"right.bin"
            li=root/"left.tsv"; ri=root/"right.tsv"
            left.write_bytes(b"ABCD"); right.write_bytes(b"xxxxABCD")
            li.write_text("00006000\\t4\\t0\\tY\\tdiagnostic_dispatch\n")
            ri.write_text("00006004\\t4\\t0\\t-\\tFUN_6004\n")
            parity=compare(left,li,right,ri,0x6000,0x6000)
            result=build_projection(parity,"LEFT","RIGHT")
            self.assertEqual(result["counts"]["eligible"],1)
            self.assertEqual(result["projections"][0]["source_name"],"diagnostic_dispatch")
            self.assertFalse(result["policy"]["automatic_application_authorized"])

    def test_different_existing_names_are_a_conflict(self):
        parity={"unique_identical_body_mappings":[{
            "left_address":"0x1","right_address":"0x2","size":8,"sha256":"abc",
            "left_name":"one","right_name":"two","left_named":True,"right_named":True,
        }]}
        result=build_projection(parity,"A","B")
        self.assertEqual(result["counts"]["conflict"],1)

    def test_address_suffix_names_are_not_semantic_conflicts(self):
        parity={"unique_identical_body_mappings":[{
            "left_address":"0x100","right_address":"0x240","size":8,"sha256":"abc",
            "left_name":"interpolate_100","right_name":"interpolate_240",
            "left_named":True,"right_named":True,
        }]}
        result=build_projection(parity,"A","B")
        self.assertEqual(result["counts"]["address_variant"],1)
        self.assertEqual(result["projections"][0]["canonical_name"],"interpolate")


if __name__ == "__main__":
    unittest.main()
