// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Tiny JSON helpers. We don't pull in a full JSON library because the
// shipping wire formats (audit.log, st::diff envelopes, AI backend
// requests, CLI --json verbs) are all writer-controlled and use a
// fixed-key shape. A 30-line escaper covers the entire write side;
// hand-rolled parsers in each consumer cover the rare read side
// (audit.cpp parses its own entries; backend.cpp extracts a single
// nested value from provider responses).
//
// One source of truth for the escaper matters because every module
// emits to the same on-disk format spec — a typo in one copy would
// silently produce parser-incompatible bytes.

#ifndef ST_CORE_JSON_UTIL_HPP
#define ST_CORE_JSON_UTIL_HPP

#include <string>
#include <string_view>

namespace st {

// Append `s` to `out` as a JSON-quoted string. Wraps the value in
// surrounding double quotes and escapes the JSON-required subset:
// `"`, `\`, `\n`, `\r`, `\t`, and any control character below 0x20
// as `\u00XX`. Bytes ≥ 0x20 (including UTF-8 continuation bytes)
// pass through verbatim — the on-disk encoding stays UTF-8.
//
// Pure / allocation-only-into-out / noexcept guaranteed apart from
// the append-might-throw-bad_alloc contract of std::string.
void json_escape(std::string &out, std::string_view s);

} // namespace st

#endif // ST_CORE_JSON_UTIL_HPP
