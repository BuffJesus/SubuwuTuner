import tempfile
import unittest
from pathlib import Path

from tools.rom_diag_record_candidates import recover


class RomDiagRecordCandidatesTests(unittest.TestCase):
    def test_recovers_stride_index_and_validates_known_name(self):
        with tempfile.TemporaryDirectory() as raw:
            path=Path(raw)/"decomp_0.txt"
            path.write_text(
                "==== diag_monitor_mature_rec04 @ 00000100 size=48 xrefs=1 ====\n"
                "puVar1 = PTR_DAT_00000180;\n"
                "if ((PTR[0x32] & 1) != 0) { puVar1[0x32] = puVar1[0x32] | 4; }\n",
                encoding="utf-8",
            )
            result=recover([path],"TEST")
            self.assertEqual(result["known_validation_count"],1)
            self.assertEqual(result["inconsistency_count"],0)
            self.assertEqual(result["known"][0]["record_index"],4)

    def test_unnamed_function_becomes_review_candidate(self):
        with tempfile.TemporaryDirectory() as raw:
            path=Path(raw)/"decomp_0.txt"
            path.write_text(
                "==== FUN_200 @ 00000200 size=48 xrefs=1 ====\n"
                "puVar1 = PTR_DAT_00000280;\n"
                "if ((PTR[0x3e] & 1) != 0) { puVar1[0x3e] = puVar1[0x3e] | 4; }\n",
                encoding="utf-8",
            )
            result=recover([path],"TEST")
            self.assertEqual(result["candidate_count"],1)
            self.assertEqual(result["candidates"][0]["proposed_name"],"diag_monitor_mature_rec05")


if __name__ == "__main__":
    unittest.main()
