// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/core/error.hpp"

#include <string>

namespace st {

std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Ok:
        return "ok";
    case ErrorCode::Unknown:
        return "unknown error";
    case ErrorCode::InvalidArgument:
        return "invalid argument";
    case ErrorCode::OutOfRange:
        return "out of range";
    case ErrorCode::NotImplemented:
        return "not implemented";
    case ErrorCode::Cancelled:
        return "cancelled";
    case ErrorCode::IoFailure:
        return "I/O failure";
    case ErrorCode::FileNotFound:
        return "file not found";
    case ErrorCode::PermissionDenied:
        return "permission denied";
    case ErrorCode::UnexpectedEof:
        return "unexpected end of file";
    case ErrorCode::ParseError:
        return "parse error";
    case ErrorCode::BadMagic:
        return "bad magic / signature";
    case ErrorCode::BadChecksum:
        return "checksum mismatch";
    case ErrorCode::UnsupportedVersion:
        return "unsupported version";
    case ErrorCode::TransportUnavailable:
        return "transport unavailable";
    case ErrorCode::TransportTimeout:
        return "transport timeout";
    case ErrorCode::TransportNack:
        return "transport negative-acknowledged";
    case ErrorCode::ProtocolError:
        return "transport protocol error";
    case ErrorCode::EcuRejected:
        return "ECU rejected request";
    case ErrorCode::SeedKeyFailed:
        return "seed/key authentication failed";
    case ErrorCode::PolicyDenied:
        return "policy denied";
    }
    return "unrecognised error code";
}

std::string Error::to_string() const {
    std::string out{st::to_string(code_)};
    if (!message_.empty()) {
        out.append(": ");
        out.append(message_);
    }
    return out;
}

} // namespace st
