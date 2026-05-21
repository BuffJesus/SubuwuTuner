// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::transport::parse_uds_trace — parse the `> req` / `< resp` trace
// format the CLI's `--trace` and `flash-trace` commands consume.
//
// Trace file format:
//
//   # comments after '#' are stripped
//   > 22 F1 90              # request: hex bytes, optional 0x prefix
//   < 62 F1 90 01 02 03     # response: hex bytes
//   > 22 F1 80              # next exchange...
//   < 62 F1 80 04 05 06
//
// Lines must alternate request → response. Each line has one direction
// marker (`>` or `<`) followed by whitespace-separated hex bytes.
//
// Used by:
//   - CLI: `rom-pull --trace`, `flash-apply --trace`, `project-flash --trace`.
//   - GUI (Trace test mode): smoke-test the Read flow without hardware.
//   - Anywhere that wants to feed a recorded ECU exchange into a
//     MockTransport via expect_send_recv.

#ifndef ST_TRANSPORT_UDS_TRACE_HPP
#define ST_TRANSPORT_UDS_TRACE_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace st::transport {

// One request/response exchange from a trace file.
struct UdsTracePair {
    std::vector<std::uint8_t> request;
    std::vector<std::uint8_t> response;
};

// Parse a trace file. On success returns true and appends to `out_pairs`.
// On failure returns false and writes a human-readable diagnostic to
// `err` (prefixed with the failing file/line context).
//
// The function is forgiving on:
//   - leading whitespace on direction-marker lines
//   - `0x` / `0X` prefix on hex bytes
//   - blank lines + comment-only lines (anything after `#`)
//
// And strict on:
//   - direction must alternate (`>` then `<`, repeat)
//   - each request and each response must contain at least one byte
//   - the file must end on a complete pair (no dangling request)
//   - the file must contain at least one exchange
//
// Empty `out_pairs` is treated as an error (callers want pairs to feed
// to MockTransport — an empty trace would be a no-op masquerading as a
// dataset, so reject it loudly).
bool parse_uds_trace(std::filesystem::path const &path,
                     std::vector<UdsTracePair> &out_pairs,
                     std::string &err);

} // namespace st::transport

#endif // ST_TRANSPORT_UDS_TRACE_HPP
