// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Defensive validators for strings that flow into a process-spawn sink
// (`std::system`, `_popen`, `popen`, …). SubuwuTuner only spawns three
// kinds of commands today:
//
//   1. `curl` invocations in the AI backend (`ai/src/backend.cpp`)
//   2. `explorer /select,…` / `open -R …` / `xdg-open …` reveal-in-OS
//      helpers in the audit panel and settings modal
//   3. Same reveal helpers in any future "open in file browser" button
//
// For all three the command string is built by concatenating a hard-
// coded program name with caller-controlled strings (API keys, project
// paths, config paths). Shell injection would require the caller to
// land an unescaped metacharacter into one of those strings — for the
// curl case that's a maliciously-crafted API key, for the reveal case
// it's a project directory whose name contains a shell metacharacter
// that the user picked from the file dialog.
//
// Neither realistic attacker can reach this code without first walking
// past `validate_ptm_metadata_field` on the .ptm path, but the same
// belt-+-suspenders rationale that motivated that validator applies
// here: the cost of a one-line check is trivial and the failure mode
// of getting it wrong is "arbitrary code execution on the analyst's
// workstation". This matches the architectural posture documented at
// `findings/tuning-knowledge-2026-06-13/cmd22-path-re/shell_injection_sinks.md`.
//
// Two validators because the legitimate character set differs:
//
//   * `validate_shell_safe_path` is for paths headed into a `system()`
//     command line. Filesystem paths can legitimately contain spaces,
//     `.`, `_`, `-`, `:` (Windows drive letter), `/` and `\` (separa-
//     tors), `(` and `)` (Windows "Program Files (x86)"), and `'`
//     (Apple's HOME directory under some user-account naming schemes).
//     We reject the metacharacters that would actually break out of
//     the double-quoted command-line argument we wrap the path in:
//     `"`, `` ` ``, `$`, newline, carriage-return, `|`, `;`, `&`, `<`,
//     `>`, `\0`. We do NOT reject `'` because the reveal commands wrap
//     in `"` not `'`.
//
//   * `validate_api_key` is for AI provider API keys that get
//     concatenated into a `-H "x-api-key: …"` header. Real provider
//     keys are alphanumeric plus `-` and `_` (Anthropic: `sk-ant-…`,
//     OpenAI: `sk-…`). Anything outside that set is either a typo or
//     an injection attempt.

#ifndef ST_CORE_SHELL_SAFE_HPP
#define ST_CORE_SHELL_SAFE_HPP

#include "st/core/result.hpp"

#include <string_view>

namespace st {

// Validate a path that is about to be embedded in a double-quoted
// argument of a `std::system()` command line. Returns InvalidArgument
// with a description naming the offending byte + offset if any of the
// following are present: NUL, LF, CR, `"`, `` ` ``, `$`, `|`, `;`,
// `&`, `<`, `>`. Empty path is accepted (caller is responsible for
// deciding whether empty is meaningful in its context).
//
// `field_name` is interpolated into the error message so a caller
// using this for multiple distinct fields can disambiguate.
[[nodiscard]] Status
validate_shell_safe_path(std::string_view field_name, std::string_view path);

// Validate an AI-provider API key. Allowed: ASCII alphanumeric plus
// `-`, `_`, `.`. Capped at 512 characters (real provider keys are
// well under 200; the cap defends against pathological inputs that
// would blow out the command-line buffer). Empty key is accepted by
// this validator — the AI backend rejects empty keys separately as a
// "you haven't configured a key" error rather than a security error.
[[nodiscard]] Status
validate_api_key(std::string_view provider, std::string_view key);

} // namespace st

#endif // ST_CORE_SHELL_SAFE_HPP
