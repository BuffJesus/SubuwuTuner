import tempfile
import unittest
from pathlib import Path

from tools.rom_subsystem_seams import inventory


class RomSubsystemSeamsTests(unittest.TestCase):
    def test_default_does_not_match_fault_anchor(self):
        from tools.rom_subsystem_seams import anchor_subsystems
        self.assertNotIn("diagnostics",anchor_subsystems("clamp_sensor_or_default"))

    def test_direct_edges_create_evidence_without_renaming(self):
        with tempfile.TemporaryDirectory() as raw:
            root=Path(raw); index=root/"index.txt"; decomp=root/"decomp_0.txt"
            index.write_text(
                "00000100\\t8\\t1\\t-\\tFUN_100\n"
                "00000200\\t8\\t1\\tY\\tdiag_dispatch\n"
                "00000300\\t8\\t1\\tY\\tdtc_monitor\n",
                encoding="utf-8",
            )
            decomp.write_text(
                "==== FUN_100 @ 00000100 size=8 xrefs=1 ====\n"
                "calls: diag_dispatch@00000200 dtc_monitor@00000300\n",
                encoding="utf-8",
            )
            result=inventory(index,[decomp],"TEST")
            self.assertEqual(result["candidate_count"],1)
            candidate=result["candidates"][0]
            self.assertEqual(candidate["subsystem"],"diagnostics")
            self.assertEqual(candidate["confidence"],"multi_anchor")
            self.assertFalse(result["policy"]["renaming_authorized"])

    def test_named_caller_can_expose_unnamed_callee(self):
        with tempfile.TemporaryDirectory() as raw:
            root=Path(raw); index=root/"index.txt"; decomp=root/"decomp_0.txt"
            index.write_text(
                "00000100\\t8\\t1\\tY\\tchecksum_verify\n"
                "00000200\\t8\\t1\\t-\\tFUN_200\n",
                encoding="utf-8",
            )
            decomp.write_text(
                "==== checksum_verify @ 00000100 size=8 xrefs=1 ====\n"
                "calls: FUN_200@00000200\n",
                encoding="utf-8",
            )
            result=inventory(index,[decomp],"TEST")
            self.assertEqual(result["candidates"][0]["subsystem"],"checksum_integrity")
            self.assertEqual(result["candidates"][0]["evidence"][0]["direction"],"called_by_named_anchor")


if __name__ == "__main__":
    unittest.main()
