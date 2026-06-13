// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// COBB AccessPort V3 datalogger preset format — `/presets/*.cfg`
// (950 B – 5 KB ASCII INI files containing the user's saved logger
// configurations). Decoded by analyst RE4 (2026-06-12 PM, findings
// tree). The format is a 3-key ASCII INI with CRLF line endings:
//
//   preset_name=<display name>\r\n
//   preset_description=<freeform description>\r\n
//   log_mon_list=<id>,<id>,...,<id>\r\n
//
// `log_mon_list` is a comma-separated list of monitor identifiers
// (the AP firmware's internal symbol set — overlaps with the OEM
// SSM 0xA8 RAM-poll catalog plus COBB's custom mode 0x22 DIDs).
//
// SubuwuTuner can read these to import the user's existing presets
// (e.g. when building a v1.1 logger gauge cluster) and write them
// back so SubuwuTuner-authored datalog configs can round-trip to
// the AccessPort.

#ifndef ST_DEVICES_AP3_DATALOGGER_CFG_HPP
#define ST_DEVICES_AP3_DATALOGGER_CFG_HPP

#include "st/core/result.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace st::devices::ets {

struct DataloggerPreset {
    std::string name;
    std::string description;
    std::vector<std::string> log_mon_list;
};

// Parse a `.cfg` preset from its raw byte contents. Tolerates both
// CRLF and bare-LF line endings (the AP firmware emits CRLF; tools
// editing the file on POSIX hosts may drop the CR). Returns
// ParseError on a missing required key (`preset_name` MUST be set;
// the other two are optional and default to empty). Whitespace
// around `=` is trimmed.
[[nodiscard]] Result<DataloggerPreset>
decode_datalogger_cfg(std::string_view text);

// Serialize a preset to the AP-canonical wire format (CRLF line
// endings; keys in the order the AP firmware emits them). The
// output is always plain ASCII — non-ASCII bytes in `name` /
// `description` / `log_mon_list[]` are passed through; the AP
// firmware's tolerance for those is undocumented, so callers should
// validate upstream.
[[nodiscard]] std::string encode_datalogger_cfg(DataloggerPreset const &p);

} // namespace st::devices::ets

#endif // ST_DEVICES_AP3_DATALOGGER_CFG_HPP
