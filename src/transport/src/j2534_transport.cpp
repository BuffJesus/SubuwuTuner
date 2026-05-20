// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/transport/j2534_transport.hpp"

#include "st/core/error.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>

namespace st::transport::j2534 {

Result<ConnectParams> connect_params_for(LinkConfig const &cfg) noexcept {
    ConnectParams p{};
    switch (cfg.kind) {
    case LinkKind::KLine:
        p.protocol = ProtocolId::ISO9141;
        p.flags = 0; // checksum on, no extra padding
        break;
    case LinkKind::CanIso15765:
        p.protocol = ProtocolId::ISO15765;
        p.flags = 0; // 11-bit CAN id (Subaru); 29-bit needs 0x100
        break;
    case LinkKind::CanFd:
        return failure(ErrorCode::InvalidArgument, "j2534::connect_params_for: CAN-FD is not "
                                                   "in J2534 v04.04; needs a v05.00-capable "
                                                   "adapter, which we don't target yet");
    }
    if (cfg.baud <= 0) {
        return failure(ErrorCode::InvalidArgument, "j2534::connect_params_for: LinkConfig.baud "
                                                   "must be positive");
    }
    p.baud = static_cast<u32>(cfg.baud);
    return p;
}

st::Status to_st_status(Status s, std::string_view context) noexcept {
    if (s == Status::NoError) {
        return ok();
    }
    ErrorCode code = ErrorCode::Unknown;
    switch (s) {
    case Status::Timeout:
    case Status::BufferEmpty:
        code = ErrorCode::TransportTimeout;
        break;
    case Status::DeviceNotConnected:
        code = ErrorCode::TransportUnavailable;
        break;
    case Status::NotSupported:
    case Status::InvalidProtocolId:
    case Status::InvalidBaudrate:
    case Status::InvalidFlags:
    case Status::InvalidIoctlValue:
    case Status::InvalidIoctlId:
    case Status::InvalidChannelId:
    case Status::InvalidDeviceId:
    case Status::InvalidFilterId:
    case Status::InvalidMsg:
    case Status::InvalidMsgId:
    case Status::InvalidTimeInterval:
    case Status::NullParameter:
    case Status::PinInvalid:
    case Status::MsgProtocolId:
        code = ErrorCode::InvalidArgument;
        break;
    default:
        code = ErrorCode::TransportNack;
        break;
    }
    std::string msg;
    if (!context.empty()) {
        msg.append(context);
        msg.append(": ");
    }
    auto const sm = status_message(s);
    msg.append(sm.data(), sm.size());
    return failure(code, std::move(msg));
}

Transport::Transport(J2534Library library) noexcept : library_(std::move(library)) {}

Transport::~Transport() {
    if (open_) {
        // Best-effort cleanup. Ignore errors — destructor must not
        // throw. The DLL is in an undefined state if these fail, but
        // the OS will reclaim the device handle on process exit.
        (void)close();
    }
}

st::Status Transport::open(LinkConfig const &cfg) {
    if (open_) {
        return failure(ErrorCode::InvalidArgument,
                       "j2534::Transport::open: already open — close() first");
    }
    auto const &fn = library_.functions();
    if (!fn.core_complete()) {
        return failure(ErrorCode::TransportUnavailable,
                       "j2534::Transport::open: J2534Library function "
                       "table is missing a core entry point — vendor "
                       "DLL is incomplete or wasn't fully resolved");
    }

    auto params = connect_params_for(cfg);
    if (!params.has_value()) {
        return failure(params.error());
    }

    // PassThruOpen — vendor name is optional per the spec; we pass
    // nullptr to let the DLL pick its default device. Multi-adapter
    // hosts disambiguate via the device name; we'll plumb it through
    // when adapter discovery lands.
    if (auto const s = fn.open(nullptr, &device_id_); s != Status::NoError) {
        return to_st_status(s, "PassThruOpen");
    }

    // PassThruConnect — negotiate the link.
    if (auto const s = fn.connect(device_id_, static_cast<u32>(params->protocol), params->flags,
                                  params->baud, &channel_id_);
        s != Status::NoError) {
        // Roll back the open() so the next attempt starts clean.
        (void)fn.close(device_id_);
        device_id_ = 0;
        return to_st_status(s, "PassThruConnect");
    }

    // Cache firmware version for the firmware() inspection path.
    // Diagnostics-only — a DLL that doesn't supply ReadVersion is
    // acceptable (the J2534Library readiness check explicitly
    // excludes ReadVersion + GetLastError).
    firmware_.clear();
    if (fn.read_version != nullptr) {
        char firmware_buf[80] = {};
        char dll_buf[80] = {};
        char api_buf[80] = {};
        if (fn.read_version(device_id_, firmware_buf, dll_buf, api_buf) == Status::NoError) {
            firmware_.assign(firmware_buf);
        }
    }

    protocol_ = params->protocol;
    open_ = true;
    return ok();
}

st::Status Transport::close() {
    if (!open_) {
        return ok(); // idempotent per ITransport contract
    }
    auto const &fn = library_.functions();
    auto const dc = fn.disconnect(channel_id_);
    auto const cl = fn.close(device_id_);
    open_ = false;
    channel_id_ = 0;
    device_id_ = 0;
    firmware_.clear();
    // If both calls failed, report the disconnect failure — it's the
    // more interesting one (close() usually succeeds even when the
    // device is gone).
    if (dc != Status::NoError)
        return to_st_status(dc, "PassThruDisconnect");
    if (cl != Status::NoError)
        return to_st_status(cl, "PassThruClose");
    return ok();
}

namespace {

// Build a single outgoing PassThruMsg. Payload size must fit in the
// fixed-size Data array (4128 bytes per the spec); we reject larger
// payloads up-front so the DLL never sees a malformed message.
[[nodiscard]] Result<PassThruMsg> build_msg(ProtocolId protocol,
                                            std::span<std::uint8_t const> payload) {
    constexpr std::size_t kMaxPayload = sizeof(PassThruMsg::data);
    if (payload.size() > kMaxPayload) {
        std::string m{"j2534::Transport: payload size "};
        m.append(std::to_string(payload.size()));
        m.append(" exceeds PassThruMsg.Data capacity of ");
        m.append(std::to_string(kMaxPayload));
        return failure(ErrorCode::InvalidArgument, std::move(m));
    }
    PassThruMsg msg{};
    msg.protocol_id = static_cast<u32>(protocol);
    msg.data_size = static_cast<u32>(payload.size());
    std::memcpy(msg.data.data(), payload.data(), payload.size());
    return msg;
}

} // namespace

Result<Frame> Transport::send_recv(std::span<std::uint8_t const> payload,
                                   std::chrono::milliseconds timeout) {
    if (!open_) {
        return failure(ErrorCode::TransportUnavailable,
                       "j2534::Transport::send_recv: transport not open");
    }
    auto msg_r = build_msg(protocol_, payload);
    if (!msg_r.has_value())
        return failure(msg_r.error());

    auto const &fn = library_.functions();
    PassThruMsg tx = *msg_r;
    u32 tx_count = 1;
    // Same timeout budget for write + read. The DLL's per-call
    // timeout means "how long to spend inside this call"; the
    // caller's `timeout` is the wall-clock budget for the whole
    // exchange. We use it for both — the write should be near-
    // instant on USB but a stalled adapter shouldn't burn the
    // whole budget there.
    auto const t_ms = static_cast<u32>(std::max<std::int64_t>(0, timeout.count()));
    if (auto const s = fn.write_msgs(channel_id_, &tx, &tx_count, t_ms); s != Status::NoError) {
        // to_st_status returns Status (Result<void>); send_recv
        // returns Result<Frame>, so unwrap the Error and re-wrap.
        return failure(to_st_status(s, "PassThruWriteMsgs").error());
    }

    // Read loop. The DLL may return BufferEmpty when polled before
    // the ECU's reply has landed; treat that as "still waiting" and
    // re-poll until our host-side deadline runs out. This mirrors
    // the wait pattern RR's serial driver uses (per the
    // RomRaider+SubaruDefs findings doc this session committed).
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        PassThruMsg rx{};
        u32 rx_count = 1;
        auto const now = std::chrono::steady_clock::now();
        auto const remaining_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        auto const poll_ms = static_cast<u32>(std::max<std::int64_t>(0, remaining_ms.count()));
        auto const s = fn.read_msgs(channel_id_, &rx, &rx_count, poll_ms);
        if (s == Status::NoError && rx_count >= 1) {
            Frame f;
            f.data.assign(rx.data.data(), rx.data.data() + rx.data_size);
            f.arrived = std::chrono::steady_clock::now();
            return f;
        }
        // BufferEmpty means "no message yet — keep polling." Anything
        // else (incl. Timeout from the DLL itself) is fatal for this
        // exchange.
        if (s != Status::BufferEmpty) {
            return failure(to_st_status(s, "PassThruReadMsgs").error());
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return failure(ErrorCode::TransportTimeout, "j2534::Transport::send_recv: no response "
                                                        "before deadline");
        }
    }
}

st::Status Transport::send(std::span<std::uint8_t const> payload) {
    if (!open_) {
        return failure(ErrorCode::TransportUnavailable,
                       "j2534::Transport::send: transport not open");
    }
    auto msg_r = build_msg(protocol_, payload);
    if (!msg_r.has_value())
        return failure(msg_r.error());

    auto const &fn = library_.functions();
    PassThruMsg tx = *msg_r;
    u32 tx_count = 1;
    auto const s = fn.write_msgs(channel_id_, &tx, &tx_count, 0);
    if (s != Status::NoError) {
        return to_st_status(s, "PassThruWriteMsgs");
    }
    return ok();
}

st::Status Transport::start_streaming(FrameCallback /*callback*/) {
    return failure(ErrorCode::NotImplemented, "j2534::Transport::start_streaming: streaming "
                                              "lands with the datalogger I/O thread + ring "
                                              "buffer (Phase 3 follow-up; see docs/13)");
}

st::Status Transport::stop_streaming() {
    return failure(ErrorCode::NotImplemented,
                   "j2534::Transport::stop_streaming: see start_streaming");
}

} // namespace st::transport::j2534
