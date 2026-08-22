import tempfile, unittest
from pathlib import Path
from tools.rom_sibling_fingerprint import changed_runs, compare

class SiblingFingerprintTests(unittest.TestCase):
    def test_changed_runs(self):
        self.assertEqual(changed_runs(b"abcdef", b"abXYef"), [(2,4)])
        self.assertEqual(changed_runs(b"abc", b"abcde"), [(3,5)])

    def test_compare(self):
        with tempfile.TemporaryDirectory() as raw:
            root=Path(raw); base=root/"base.bin"; same=root/"same.bin"; changed=root/"changed.bin"
            base.write_bytes(b"abcdef"); same.write_bytes(b"abcdef"); changed.write_bytes(b"abXYef")
            rows=compare(base,[same,changed])["siblings"]
            self.assertTrue(rows[0]["identical"]); self.assertEqual(rows[1]["changed_bytes"],2)
            self.assertEqual(rows[1]["runs"][0],{"start":2,"end":4,"length":2})

if __name__ == "__main__": unittest.main()
