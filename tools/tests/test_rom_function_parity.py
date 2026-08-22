import tempfile, unittest
from pathlib import Path
from tools.rom_function_parity import compare

class FunctionParityTests(unittest.TestCase):
    def test_changed_body(self):
        with tempfile.TemporaryDirectory() as raw:
            r=Path(raw); a=r/"a.bin"; b=r/"b.bin"; ai=r/"a.txt"; bi=r/"b.txt"
            a.write_bytes(b"00abcd99"); b.write_bytes(b"00abXd99")
            row="00000002\\t4\\t1\\t-\\tFUN_2\n"; ai.write_text(row); bi.write_text(row)
            result=compare(a,ai,b,bi)
            self.assertEqual(result["changed_bodies"],["0x2"])

    def test_unique_relocated_identical_body(self):
        with tempfile.TemporaryDirectory() as raw:
            r=Path(raw); a=r/"a.bin"; b=r/"b.bin"; ai=r/"a.txt"; bi=r/"b.txt"
            a.write_bytes(b"ABCDxxxx"); b.write_bytes(b"xxxxABCD")
            ai.write_text("00000000\\t4\\t0\\tY\\tleft_name\n")
            bi.write_text("00000004\\t4\\t0\\t-\\tFUN_4\n")
            result=compare(a,ai,b,bi)
            self.assertEqual(result["shared_body_hashes_any_address"],1)
            match=result["unique_relocated_identical_bodies"][0]
            self.assertEqual(match["left_address"],"0x0")
            self.assertEqual(match["right_address"],"0x4")

    def test_maps_virtual_addresses_through_image_base(self):
        with tempfile.TemporaryDirectory() as raw:
            r=Path(raw); a=r/"a.bin"; b=r/"b.bin"; ai=r/"a.txt"; bi=r/"b.txt"
            a.write_bytes(b"ABCD"); b.write_bytes(b"ABCD")
            row="00006000\\t4\\t0\\t-\\tFUN_6000\n"
            ai.write_text(row); bi.write_text(row)
            result=compare(a,ai,b,bi,0x6000,0x6000)
            self.assertEqual(result["identical_bodies"],1)

if __name__=="__main__": unittest.main()
