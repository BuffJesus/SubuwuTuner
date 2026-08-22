import struct
import tempfile
import unittest
from pathlib import Path

from tools.rom_pointer_aliases import audit


class RomPointerAliasesTests(unittest.TestCase):
    def test_maps_literal_to_referencing_function(self):
        blob = bytearray(0x40)
        struct.pack_into(">I", blob, 0x10, 0xFFF80020)
        with tempfile.TemporaryDirectory() as raw:
            path = Path(raw) / "decomp_test.txt"
            path.write_text(
                "==== writer @ 00006000 size=4 xrefs=1 ====\n"
                "*(undefined2 *)PTR_DAT_00006010 = 1;\n", encoding="utf-8")
            result = audit(bytes(blob), [path], [0xFFF80020], 0x6000)
        target = result["targets"]["0xFFF80020"]
        self.assertEqual(target["literal_count"], 1)
        self.assertEqual(target["aliases"][0]["references"][0]["function"], "writer")


if __name__ == "__main__":
    unittest.main()
