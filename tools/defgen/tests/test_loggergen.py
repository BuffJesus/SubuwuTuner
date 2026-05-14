# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""Unit tests for loggergen. Run with: python -m unittest discover -s tools/defgen"""

from __future__ import annotations

import sys
import tomllib
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import loggergen  # noqa: E402


def _wrap(parameters_xml: str) -> str:
    """Wrap one or more <parameter> elements in the minimal logger envelope."""
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<logger>
  <protocols>
    <protocol id="SSM">
      <parameters>
        {parameters_xml}
      </parameters>
      <switches/>
      <ecuparams/>
    </protocol>
  </protocols>
</logger>"""


class LoggergenParseTest(unittest.TestCase):
    def test_single_byte_pid(self):
        xml = _wrap('''
          <parameter id="P2" name="Coolant Temperature" ecubyteindex="8" ecubit="6">
            <address>0x000008</address>
            <conversions>
              <conversion units="C" expr="x-40" format="0"
                          gauge_min="-20" gauge_max="120" gauge_step="20"/>
              <conversion units="F" expr="32+9*(x-40)/5" format="0"/>
            </conversions>
          </parameter>
        ''')
        scalings, pids, warnings = loggergen.parse_logger_xml(xml)
        self.assertEqual(warnings, [])
        self.assertEqual(len(pids), 1)
        pid = pids[0]
        self.assertEqual(pid["id"], "p2")
        self.assertEqual(pid["name"], "Coolant Temperature")
        self.assertEqual(pid["ssm_address"], "0x00000008")
        self.assertEqual(pid["length"], 1)
        self.assertEqual(pid["data_type"], "uint8")
        self.assertEqual(pid["unit"], "C")
        # Single scaling, math reduces exactly.
        self.assertEqual(len(scalings), 1)
        self.assertEqual(scalings[0].factor, 1.0)
        self.assertEqual(scalings[0].offset, -40.0)

    def test_multi_byte_pid_picks_uint16_be(self):
        xml = _wrap('''
          <parameter id="P8" name="Engine Speed">
            <address length="2">0x00000E</address>
            <conversions>
              <conversion units="rpm" expr="x/4" format="0"/>
            </conversions>
          </parameter>
        ''')
        _, pids, _ = loggergen.parse_logger_xml(xml)
        self.assertEqual(pids[0]["length"], 2)
        self.assertEqual(pids[0]["data_type"], "uint16_be")
        self.assertEqual(pids[0]["ssm_address"], "0x0000000E")

    def test_scaling_dedup_across_pids(self):
        # Three A/F-correction PIDs share the identical `(x-128)*100/128`
        # scaling — should collapse to one [[scaling]] record.
        xml = _wrap('''
          <parameter id="P3" name="A/F Correction #1">
            <address>0x000009</address>
            <conversions><conversion units="%" expr="(x-128)*100/128" format="0.00"/></conversions>
          </parameter>
          <parameter id="P4" name="A/F Learning #1">
            <address>0x00000A</address>
            <conversions><conversion units="%" expr="(x-128)*100/128" format="0.00"/></conversions>
          </parameter>
          <parameter id="P5" name="A/F Correction #2">
            <address>0x00000B</address>
            <conversions><conversion units="%" expr="(x-128)*100/128" format="0.00"/></conversions>
          </parameter>
        ''')
        scalings, pids, _ = loggergen.parse_logger_xml(xml)
        self.assertEqual(len(pids), 3)
        self.assertEqual(len(scalings), 1)
        # All three PIDs reference the same scaling id.
        self.assertEqual({p["scaling"] for p in pids}, {scalings[0].id})
        # Math: factor=100/128, offset=-100.
        self.assertAlmostEqual(scalings[0].factor, 100.0 / 128.0)
        self.assertAlmostEqual(scalings[0].offset, -100.0)

    def test_pid_without_address_is_skipped_with_warning(self):
        xml = _wrap('''
          <parameter id="P200" name="Derived (no address)">
            <conversions><conversion units="" expr="x" format="0"/></conversions>
          </parameter>
        ''')
        _, pids, warnings = loggergen.parse_logger_xml(xml)
        self.assertEqual(pids, [])
        self.assertEqual(len(warnings), 1)
        self.assertEqual(warnings[0][:2], ("pid", "p200"))
        self.assertIn("no <address>", warnings[0][2])

    def test_non_linear_conversion_flattens_with_warning(self):
        # `(x-128)*100/x` is non-linear (x in the denominator).
        xml = _wrap('''
          <parameter id="P99" name="Bogus">
            <address>0x000020</address>
            <conversions><conversion units="%" expr="(x-128)*100/x" format="0.00"/></conversions>
          </parameter>
        ''')
        scalings, pids, warnings = loggergen.parse_logger_xml(xml)
        self.assertEqual(len(pids), 1)
        self.assertEqual(scalings[0].factor, 1.0)
        self.assertEqual(scalings[0].offset, 0.0)
        self.assertEqual(len(warnings), 1)
        self.assertEqual(warnings[0][:2], ("pid", "p99"))
        self.assertIn("non-linear", warnings[0][2])

    def test_emit_pack_toml_round_trips(self):
        xml = _wrap('''
          <parameter id="P1" name="Engine Load (Relative)">
            <address>0x000007</address>
            <conversions><conversion units="%" expr="x*100/255" format="0.00"/></conversions>
          </parameter>
        ''')
        scalings, pids, _ = loggergen.parse_logger_xml(xml)
        text = loggergen.emit_pack_toml(scalings, pids)
        raw  = tomllib.loads(text)
        self.assertEqual(raw["pack"]["id"], "subaru-ssm-pids")
        self.assertEqual(raw["pack"]["rom_size_bytes"], 0)
        self.assertEqual(len(raw["pid"]), 1)
        self.assertEqual(raw["pid"][0]["id"], "p1")
        self.assertEqual(raw["pid"][0]["ssm_address"], 0x07)


if __name__ == "__main__":
    unittest.main()
