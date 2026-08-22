import tempfile
import unittest
from pathlib import Path

from tools.rom_decompile_baseline import audit, parse_index


class RomDecompileBaselineTests(unittest.TestCase):
    def test_distinguishes_source_bytes_from_modeled_memory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rom = root / "rom.bin"
            index = root / "function_index_named.txt"
            rom.write_bytes(bytes(range(32)))
            index.write_text(
                "# program=ROM.bin lang=SuperH:BE:32:SH-2A base=00000000 size=96\n"
                "# addr\\tsize\\txrefs_to\\tnamed\\tname\n"
                "00006000\\t8\\t2\\t-\\tFUN_00006000\n"
                "00006008\\t4\\t1\\tY\\tmain\n"
                "ffff8000\\t4\\t0\\tY\\tregister_stub\n",
                encoding="utf-8",
            )
            result = audit(rom, index, 0x6000)
            self.assertEqual(result["source"]["size"], 32)
            self.assertEqual(result["index"]["modeled_memory_bytes"], 96)
            self.assertEqual(result["index"]["function_count"], 3)
            self.assertEqual(result["index"]["named_function_count"], 2)
            self.assertEqual(result["index"]["functions_within_source_mapping"], 2)
            self.assertEqual(result["index"]["functions_outside_source_mapping"], 1)
            self.assertFalse(result["interpretation"]["modeled_memory_is_source_size"])

    def test_rejects_malformed_rows(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "index.txt"
            path.write_text(
                "# program=x lang=y base=0 size=1\n# columns\ninvalid\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "five tab-separated"):
                parse_index(path)


if __name__ == "__main__":
    unittest.main()
