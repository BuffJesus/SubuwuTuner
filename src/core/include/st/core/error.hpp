// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_CORE_ERROR_HPP
#define ST_CORE_ERROR_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace st {

// Project-wide error code enumeration. Modules add their own codes by extending
// this enum in a follow-up patch — central registration keeps the
// `to_string()` table single-source-of-truth and prevents collisions.
enum class ErrorCode : std::uint16_t {
    Ok = 0,

    // 1xx — generic
    Unknown        = 100,
    InvalidArgument= 101,
    OutOfRange     = 102,
    NotImplemented = 103,
    Cancelled      = 104,

    // 2xx — I/O
    IoFailure      = 200,
    FileNotFound   = 201,
    PermissionDenied = 202,
    UnexpectedEof  = 203,

    // 3xx — formats (ROM, project, definitions)
    ParseError     = 300,
    BadMagic       = 301,
    BadChecksum    = 302,
    UnsupportedVersion = 303,

    // 4xx — transport (reserved for st::transport)
    TransportUnavailable = 400,
    TransportTimeout     = 401,
    TransportNack        = 402,

    // 5xx — ECU protocol (reserved for st::ecu)
    EcuRejected   = 500,
    SeedKeyFailed = 501,
};

[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

// An error has a code plus an optional human-readable message. The message
// is owned by the Error so callers can return temporaries without lifetime
// pitfalls. Errors are cheap to copy; the small-string optimisation usually
// keeps allocations out of the hot path.
class Error {
  public:
    Error() = default;

    Error(ErrorCode code, std::string message) : code_{code}, message_{std::move(message)} {}

    explicit Error(ErrorCode code) : code_{code} {}

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }

    [[nodiscard]] std::string_view message() const noexcept { return message_; }

    [[nodiscard]] std::string to_string() const;

    friend bool operator==(Error const &lhs, Error const &rhs) noexcept {
        return lhs.code_ == rhs.code_ && lhs.message_ == rhs.message_;
    }

  private:
    ErrorCode   code_{ErrorCode::Unknown};
    std::string message_{};
};

} // namespace st

#endif // ST_CORE_ERROR_HPP
