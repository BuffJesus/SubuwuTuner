import struct
import unittest

from tools.rom_interp_inventory import scan


class RomInterpInventoryTests(unittest.TestCase):
    def test_finds_monotonic_descriptor_and_summarizes_grid(self):
        blob = bytearray(0x100)
        struct.pack_into(">III2B", blob, 0x10, 0x6080, 0x6040, 0x6050, 2, 3)
        struct.pack_into(">2I", blob, 0x40, 100, 200)
        struct.pack_into(">3H", blob, 0x50, 1, 2, 3)
        struct.pack_into(">6h", blob, 0x80, -2, -1, 0, 1, 2, 2)
        struct.pack_into(">I", blob, 0x60, 0x6010)
        results = scan(bytes(blob), 0x6000)
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0]["descriptor_address"], "0x6010")
        self.assertEqual(results[0]["grid_min"], -2)
        self.assertEqual(results[0]["grid_max"], 2)
        self.assertEqual(results[0]["grid_unique_count"], 5)
        self.assertEqual(results[0]["pointer_references"], ["0x6060"])

    def test_rejects_nonmonotonic_axis(self):
        blob = bytearray(0x100)
        struct.pack_into(">III2B", blob, 0x10, 0x6080, 0x6040, 0x6050, 2, 2)
        struct.pack_into(">2I", blob, 0x40, 200, 100)
        struct.pack_into(">2H", blob, 0x50, 1, 2)
        struct.pack_into(">4h", blob, 0x80, 1, 2, 3, 4)
        self.assertEqual(scan(bytes(blob), 0x6000), [])

    def test_scans_u16_x_axis_layout(self):
        blob = bytearray(0x100)
        struct.pack_into(">III2B", blob, 0x10, 0x6080, 0x6042, 0x6050, 2, 2)
        struct.pack_into(">2H", blob, 0x42, 100, 200)
        struct.pack_into(">2H", blob, 0x50, 1, 2)
        struct.pack_into(">4h", blob, 0x80, 1, 2, 3, 4)
        results = scan(bytes(blob), 0x6000, x_type="u16")
        self.assertEqual(results[0]["x_axis_u16"], [100, 200])

    def test_rejects_grid_overlapping_axis_storage(self):
        blob = bytearray(0x100)
        struct.pack_into(">III2B", blob, 0x10, 0x6040, 0x6044, 0x6050, 2, 2)
        struct.pack_into(">2I", blob, 0x44, 100, 200)
        struct.pack_into(">2H", blob, 0x50, 1, 2)
        self.assertEqual(scan(bytes(blob), 0x6000), [])


if __name__ == "__main__":
    unittest.main()
