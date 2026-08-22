import struct
import unittest

from tools.rom_interp_descriptor import decode


class RomInterpDescriptorTests(unittest.TestCase):
    def test_decodes_big_endian_axes_and_row_major_grid(self):
        blob = bytearray(0x80)
        struct.pack_into(">III2B", blob, 0x10, 0x6040, 0x6030, 0x6038, 2, 3)
        struct.pack_into(">2I", blob, 0x30, 100, 200)
        struct.pack_into(">3H", blob, 0x38, 1, 2, 3)
        struct.pack_into(">6h", blob, 0x40, -1, 0, 1, 2, 3, 4)
        result = decode(bytes(blob), 0x6010, 0x6000)
        self.assertEqual(result["x_axis_u32"], [100, 200])
        self.assertEqual(result["y_axis_u16"], [1, 2, 3])
        self.assertEqual(result["grid_s16"], [[-1, 0, 1], [2, 3, 4]])

    def test_rejects_target_outside_mapping(self):
        blob = bytearray(0x20)
        struct.pack_into(">III2B", blob, 0, 0x7000, 0x600E, 0x6012, 1, 1)
        with self.assertRaisesRegex(ValueError, "target outside"):
            decode(bytes(blob), 0x6000, 0x6000)

    def test_decodes_u16_x_axis(self):
        blob = bytearray(0x80)
        struct.pack_into(">III2B", blob, 0x10, 0x6040, 0x6030, 0x6038, 2, 2)
        struct.pack_into(">2H", blob, 0x30, 10, 20)
        struct.pack_into(">2H", blob, 0x38, 30, 40)
        struct.pack_into(">4h", blob, 0x40, 1, 2, 3, 4)
        result = decode(bytes(blob), 0x6010, 0x6000, "u16")
        self.assertEqual(result["x_axis_u16"], [10, 20])


if __name__ == "__main__":
    unittest.main()
