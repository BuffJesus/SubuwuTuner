import tempfile
import unittest
from pathlib import Path

from tools.rom_symbol_usage import index_usage


class RomSymbolUsageTests(unittest.TestCase):
    def test_groups_case_insensitive_references_by_function(self):
        with tempfile.TemporaryDirectory() as raw:
            path = Path(raw) / "decomp_test.txt"
            path.write_text(
                "==== first @ 00001000 size=4 xrefs=1 ====\n"
                "x = DAT_ABC;\nDAT_ABC = x;\nDAT_ABCD = 0;\n"
                "==== second @ 00001004 size=4 xrefs=1 ====\n"
                "y = dat_abc;\n", encoding="utf-8")
            result = index_usage([path], ["DAT_abc"])
            rows = result["symbols"]["DAT_abc"]
            self.assertEqual([row["reference_count"] for row in rows], [2, 1])
            self.assertEqual(rows[0]["address"], "0x1000")


if __name__ == "__main__":
    unittest.main()
