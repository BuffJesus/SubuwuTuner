// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/transport/factory.hpp"

#include "st/core/error.hpp"
#include "st/transport/j2534.hpp"
#include "st/transport/j2534_transport.hpp"
#include "st/transport/native_transport.hpp"
#include "st/transport/obdx_transport.hpp"
#include "st/transport/serial_byte_channel.hpp"

#include <memory>
#include <string>
#include <utility>

namespace st::transport {

char const *kind_name(Kind k) noexcept {
    switch (k) {
    case Kind::J2534:
        return "j2534";
    case Kind::Obdx:
        return "obdx";
    case Kind::Native:
        return "native";
    }
    return "unknown";
}

std::optional<Kind> parse_kind(std::string_view s) noexcept {
    if (s == "j2534")
        return Kind::J2534;
    if (s == "obdx")
        return Kind::Obdx;
    if (s == "native")
        return Kind::Native;
    return std::nullopt;
}

Result<std::unique_ptr<ITransport>> open_transport(TransportSpec const &spec) {
    switch (spec.kind) {
    case Kind::J2534: {
        if (spec.dll_path.empty()) {
            return failure(ErrorCode::InvalidArgument, "open_transport: --transport j2534 needs "
                                                       "--dll <path> (the J2534 v04.04 vendor "
                                                       "DLL, e.g. op20pt32.dll for Tactrix "
                                                       "OpenPort 2.0).");
        }
        // J2534Library::load() returns NotImplemented today —
        // platform dynamic-load (LoadLibraryA / dlopen) lands
        // when hardware arrives. Surface its error verbatim so
        // the user sees the specific missing piece.
        auto lib = j2534::J2534Library::load(spec.dll_path);
        if (!lib.has_value())
            return failure(lib.error());
        return std::unique_ptr<ITransport>{new j2534::Transport{std::move(*lib)}};
    }
    case Kind::Obdx: {
        if (spec.device_path.empty()) {
            return failure(ErrorCode::InvalidArgument, "open_transport: --transport obdx needs "
                                                       "--device <path> (USB CDC ACM endpoint — "
                                                       "e.g. COM5 on Windows, /dev/ttyACM0 on "
                                                       "Linux).");
        }
        SerialChannelConfig cfg{};
        cfg.device_path = spec.device_path;
        cfg.baud_rate = 500000; // OBDX Pro VX default
        auto ch = make_serial_byte_channel(cfg);
        if (!ch.has_value())
            return failure(ch.error());
        return std::unique_ptr<ITransport>{new obdx::Transport{std::move(*ch)}};
    }
    case Kind::Native: {
        if (spec.device_path.empty()) {
            return failure(ErrorCode::InvalidArgument, "open_transport: --transport native needs "
                                                       "--device <path> (USB CDC ACM endpoint — "
                                                       "e.g. COM5 on Windows, /dev/ttyACM0 on "
                                                       "Linux).");
        }
        SerialChannelConfig cfg{};
        cfg.device_path = spec.device_path;
        cfg.baud_rate = 921600; // doc-18 handheld default
        auto ch = make_serial_byte_channel(cfg);
        if (!ch.has_value())
            return failure(ch.error());
        return std::unique_ptr<ITransport>{new native::Transport{std::move(*ch)}};
    }
    }
    return failure(ErrorCode::InvalidArgument,
                   "open_transport: unknown Kind value (programmer error)");
}

} // namespace st::transport
