import tempfile
import unittest
from pathlib import Path

from tools.rom_pointer_evidence import verify


class RomPointerEvidenceTests(unittest.TestCase):
    def test_verifies_big_endian_pointer(self):
        with tempfile.TemporaryDirectory() as raw:
            rom = Path(raw) / "rom.bin"
            rom.write_bytes(bytes.fromhex("12345678"))
            doc = {"schema": "subuwutuner.rom-pointer-evidence.v1", "records": [{
                "cid": "TEST", "label": "field", "pointers": [
                    {"literal_address": "0x6000", "target": "0x12345678"}]}]}
            self.assertEqual(verify(doc, {"TEST": rom}, 0x6000), [])

    def test_reports_pointer_drift(self):
        with tempfile.TemporaryDirectory() as raw:
            rom = Path(raw) / "rom.bin"; rom.write_bytes(b"\0\0\0\0")
            doc = {"schema": "subuwutuner.rom-pointer-evidence.v1", "records": [{
                "cid": "TEST", "label": "field", "pointers": [
                    {"literal_address": "0", "target": "1"}]}]}
            self.assertIn("expected 0x1", verify(doc, {"TEST": rom}, 0)[0])


if __name__ == "__main__":
    unittest.main()
