import hashlib
import unittest
from pathlib import Path

from tools.rom_semantic_evidence import verify


class RomSemanticEvidenceTests(unittest.TestCase):
    def test_verifies_body_at_virtual_address(self):
        body=b"ABCD"
        document={
            "schema":"subuwutuner.rom-semantic-evidence.v1",
            "candidates":[{
                "proposed_name":"example","size":4,
                "body_sha256":hashlib.sha256(body).hexdigest(),
                "locations":[{"cid":"TEST","address":"0x6002"}],
            }],
        }
        import tempfile
        with tempfile.TemporaryDirectory() as raw:
            rom=Path(raw)/"rom.bin"; rom.write_bytes(b"xx"+body)
            self.assertEqual(verify(document,{"TEST":rom},0x6000),[])

    def test_detects_drift(self):
        import tempfile
        with tempfile.TemporaryDirectory() as raw:
            rom=Path(raw)/"rom.bin"; rom.write_bytes(b"WXYZ")
            document={
                "schema":"subuwutuner.rom-semantic-evidence.v1",
                "candidates":[{
                    "proposed_name":"example","size":4,"body_sha256":"0"*64,
                    "locations":[{"cid":"TEST","address":"0"}],
                }],
            }
            self.assertIn("body hash mismatch",verify(document,{"TEST":rom},0)[0])


if __name__ == "__main__":
    unittest.main()
