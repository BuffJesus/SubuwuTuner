import argparse
import json
import tempfile
import unittest
from pathlib import Path

from tools import rom_re_manifest as manifest


class RomReManifestTests(unittest.TestCase):
    def test_create_pins_inputs_and_verifies(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            rom = root / "TEST100A.bin"
            labels = root / "labels.tsv"
            artifact = root / "function_index_named.txt"
            rom.write_bytes(bytes(range(256)) * 4)
            labels.write_text("address\tname\n", encoding="utf-8")
            artifact.write_text("00000100 test_fn\n", encoding="utf-8")
            args = argparse.Namespace(
                base=root, rom=rom, cid="test100a", family=None,
                processor="SuperH4:BE:32:SH-2A", endianness="big",
                image_base=0, plaintext_status="verified",
                anchor=[manifest.parse_anchor("reset:0x100:00010203")],
                labels=[labels], sibling=[], artifact=[artifact],
            )
            document = manifest.create_manifest(args)
            self.assertEqual(document["schema"], manifest.SCHEMA)
            self.assertEqual(document["family"], "TEST")
            self.assertEqual(document["source"]["size"], 1024)
            self.assertEqual(manifest.verify_manifest(document, root), [])

    def test_verify_detects_boot_anchor_drift(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            rom = root / "rom.bin"
            rom.write_bytes(b"\x10\x20\x30\x40")
            document = {
                "schema": manifest.SCHEMA,
                "source": manifest.file_record(rom, root),
                "architecture": {"image_base": 0x1000},
                "boot_anchors": [{"name": "reset", "address": 0x1000,
                                  "bytes_hex": "deadbeef"}],
                "public_label_inputs": [], "siblings": [], "artifacts": [],
            }
            errors = manifest.verify_manifest(document, root)
            self.assertTrue(any("reset mismatch" in error for error in errors))

    def test_verify_detects_source_substitution(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            rom = root / "rom.bin"
            rom.write_bytes(b"original")
            record = manifest.file_record(rom, root)
            document = {
                "schema": manifest.SCHEMA,
                "source": record,
                "public_label_inputs": [], "siblings": [], "artifacts": [],
            }
            rom.write_bytes(b"replaced")
            errors = manifest.verify_manifest(document, root)
            self.assertTrue(any("sha256 drift" in error for error in errors))

    def test_verify_rejects_unknown_schema(self):
        errors = manifest.verify_manifest({"schema": "future"}, Path("."))
        self.assertIn("unsupported schema", errors[0])


if __name__ == "__main__":
    unittest.main()
