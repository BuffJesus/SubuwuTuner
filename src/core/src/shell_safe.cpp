// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/core/shell_safe.hpp"

#include "st/core/error.hpp"

#include <cstdio>
#include <string>

namespace st {

namespace {

constexpr std::size_t kMaxApiKeyLength = 512;

[[nodiscard]] bool is_shell_unsafe_for_path(char c) noexcept {
    // Bytes that would let an attacker break out of the surrounding
    // `"…"` quoting in the `std::system()` command line. NUL would
    // truncate the C-string before `system()` even runs (which would
    // turn the intended explorer reveal into a no-op rather than RCE,
    // but defending anyway keeps the behaviour observable). CR is
    // explicitly listed because `cmd.exe` treats `\r` as a statement
    // separator on Windows even when the surrounding `"` is unbroken.
    switch (c) {
    case '\0':
    case '\n':
    case '\r':
    case '"':
    case '`':
    case '$':
    case '|':
    case ';':
    case '&':
    case '<':
    case '>':
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool is_api_key_char(char c) noexcept {
    auto const u = static_cast<unsigned char>(c);
    if (u >= 'A' && u <= 'Z')
        return true;
    if (u >= 'a' && u <= 'z')
        return true;
    if (u >= '0' && u <= '9')
        return true;
    return c == '-' || c == '_' || c == '.';
}

[[nodiscard]] std::string hex_byte(unsigned char u) {
    char buf[8];
    (void)std::snprintf(buf, sizeof(buf), "0x%02X", u);
    return std::string{buf};
}

} // namespace

Status validate_shell_safe_path(std::string_view field_name,
                                  std::string_view path) {
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (is_shell_unsafe_for_path(path[i])) {
            auto const u = static_cast<unsigned char>(path[i]);
            return failure(
                ErrorCode::InvalidArgument,
                std::string{"path field '"} + std::string{field_name} +
                    "' contains shell metacharacter " + hex_byte(u) +
                    " at offset " + std::to_string(i) +
                    " — refusing to embed in a process-spawn command line "
                    "(would break the surrounding quoting on at least one "
                    "supported platform)");
        }
    }
    return ok();
}

Status validate_api_key(std::string_view provider, std::string_view key) {
    if (key.size() > kMaxApiKeyLength) {
        return failure(ErrorCode::InvalidArgument,
                       std::string{"API key for provider '"} +
                           std::string{provider} + "' exceeds " +
                           std::to_string(kMaxApiKeyLength) + "-char cap (" +
                           std::to_string(key.size()) + " chars given)");
    }
    for (std::size_t i = 0; i < key.size(); ++i) {
        if (!is_api_key_char(key[i])) {
            auto const u = static_cast<unsigned char>(key[i]);
            return failure(
                ErrorCode::InvalidArgument,
                std::string{"API key for provider '"} + std::string{provider} +
                    "' contains disallowed character " + hex_byte(u) +
                    " at offset " + std::to_string(i) +
                    " (allowed: A-Z a-z 0-9 . _ -; rejects shell metachars "
                    "so the key can be safely embedded in the `curl -H` "
                    "argument of the AI backend's request command)");
        }
    }
    return ok();
}

} // namespace st
