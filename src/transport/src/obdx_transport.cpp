// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/transport/obdx_transport.hpp"

#include "st/core/error.hpp"
#include "st/transport/obdx_dvi.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::transport::obdx {

namespace {

using std::chrono::milliseconds;
using std::chrono::steady_clock;

// Process-wide hex-trace toggle. See header for rationale.
std::atomic<bool> g_trace_enabled{false};

// Dump a byte span to stderr as `<prefix> NN NN NN ...` (uppercase hex,
// space-separated, single line per call so log readers can grep). Caps
// the printed prefix to keep huge frames from blowing the terminal —
// SSM responses can be ~85 bytes and UDS up to a few hundred.
void trace_dump(char const *prefix, std::span<std::uint8_t const> bytes) {
    std::string line{prefix};
    line.reserve(line.size() + 4 * bytes.size() + 16);
    char buf[8];
    for (auto const b : bytes) {
        std::snprintf(buf, sizeof buf, " %02X", static_cast<unsigned>(b));
        line.append(buf);
    }
    std::fprintf(stderr, "%s (%zu B)\n", line.c_str(), bytes.size());
    std::fflush(stderr);
}

// Read exactly `n` bytes from the channel, accumulating across
// multiple read_bytes calls until `n` bytes are collected or the
// deadline passes. The byte-channel contract says read_bytes
// returns "what's available, up to max"; we re-poll until full.
[[nodiscard]] Result<std::vector<std::uint8_t>> read_exactly(IDeviceChannel &chan, std::size_t n,
                                                             milliseconds timeout) {
    std::vector<std::uint8_t> out;
    out.reserve(n);
    auto const deadline = steady_clock::now() + timeout;
    while (out.size() < n) {
        auto const remaining_ms =
            std::chrono::duration_cast<milliseconds>(deadline - steady_clock::now());
        if (remaining_ms.count() <= 0) {
            return failure(ErrorCode::TransportTimeout,
                           "obdx::read_exactly: deadline expired with " +
                               std::to_string(out.size()) + "/" + std::to_string(n) +
                               " bytes accumulated");
        }
        auto chunk = chan.read_bytes(n - out.size(), remaining_ms);
        if (!chunk.has_value())
            return failure(chunk.error());
        if (!chunk->empty()) {
            out.insert(out.end(), chunk->begin(), chunk->end());
        }
        // Empty chunk = timeout-shaped "no data this poll." Loop and
        // re-check the deadline. Real USB drivers often return early
        // with zero bytes on a short timeout — we shouldn't treat
        // that as fatal until our outer deadline runs out.
    }
    return out;
}

// Read bytes until the ELM prompt character ('>') appears or the
// deadline passes. Returns the response content (without the
// trailing '>'). ELM responses are CR-terminated ASCII; the prompt
// signals the adapter is ready for the next command.
[[nodiscard]] Result<std::string> read_until_prompt(IDeviceChannel &chan, milliseconds timeout) {
    std::string out;
    auto const deadline = steady_clock::now() + timeout;
    while (true) {
        auto const remaining_ms =
            std::chrono::duration_cast<milliseconds>(deadline - steady_clock::now());
        if (remaining_ms.count() <= 0) {
            return failure(ErrorCode::TransportTimeout, "obdx::read_until_prompt: no '>' before "
                                                        "deadline (got " +
                                                            std::to_string(out.size()) + " bytes)");
        }
        auto chunk = chan.read_bytes(64, remaining_ms);
        if (!chunk.has_value())
            return failure(chunk.error());
        for (auto b : *chunk) {
            if (b == '>') {
                return out;
            }
            out.push_back(static_cast<char>(b));
        }
    }
}

// ELM-mode command-response round trip. Writes the command with a
// trailing CR (ELM protocol convention) and reads everything up to
// the next '>' prompt. The returned string contains the adapter's
// raw response (typically a few lines of ASCII echoed back from the
// device).
[[nodiscard]] Result<std::string> elm_exchange(IDeviceChannel &chan, std::string_view cmd,
                                               milliseconds timeout) {
    std::vector<std::uint8_t> tx;
    tx.reserve(cmd.size() + 1);
    for (char c : cmd)
        tx.push_back(static_cast<std::uint8_t>(c));
    tx.push_back('\r');
    if (auto s = chan.write_bytes(tx); !s.has_value()) {
        return failure(s.error());
    }
    return read_until_prompt(chan, timeout);
}

// Read one complete DVI frame from the stream, then hand the bytes
// to obdx::dvi::decode_frame. Strategy: read 2 bytes (opcode +
// 1-byte LEN), inspect the opcode to decide whether a 2-byte LEN
// applies, read the rest of the LEN field if so, then read
// declared_len payload bytes + 1 CRC byte. Assembled buffer goes
// to the codec for CRC + structural validation.
[[nodiscard]] Result<dvi::DecodedFrame> read_dvi_frame(IDeviceChannel &chan, milliseconds timeout) {
    auto const deadline = steady_clock::now() + timeout;
    auto const remaining = [deadline]() {
        return std::max(milliseconds{0},
                        std::chrono::duration_cast<milliseconds>(deadline - steady_clock::now()));
    };

    auto head = read_exactly(chan, 2, remaining());
    if (!head.has_value())
        return failure(head.error());
    std::vector<std::uint8_t> frame = std::move(*head);

    // Is this a 2-byte-length opcode? Only RxLarge (request 0x09)
    // and TxLarge (request 0x11) carry a 2-byte LEN — their
    // responses are 0x19 / 0x11. Error frames always use 1-byte.
    std::uint8_t const opcode = frame[0];
    bool wide_length = false;
    if (opcode != dvi::kErrorOpcode) {
        std::uint8_t const req = opcode & 0xEFU;
        if (req == static_cast<std::uint8_t>(dvi::Opcode::RxLarge) ||
            req == static_cast<std::uint8_t>(dvi::Opcode::TxLarge)) {
            wide_length = true;
        }
    }
    std::size_t declared_len = 0;
    if (wide_length) {
        auto lo = read_exactly(chan, 1, remaining());
        if (!lo.has_value())
            return failure(lo.error());
        frame.push_back((*lo)[0]);
        declared_len =
            (static_cast<std::size_t>(frame[1]) << 8U) | static_cast<std::size_t>(frame[2]);
    } else {
        declared_len = frame[1];
    }
    auto rest = read_exactly(chan, declared_len + 1, remaining());
    if (!rest.has_value())
        return failure(rest.error());
    frame.insert(frame.end(), rest->begin(), rest->end());
    return dvi::decode_frame(frame);
}

// Send a DVI request, read the response frame, return the payload
// (for normal responses) or surface an error (for error frames /
// CRC failures).
[[nodiscard]] Result<std::vector<std::uint8_t>> dvi_exchange(IDeviceChannel &chan, dvi::Opcode op,
                                                             std::span<std::uint8_t const> payload,
                                                             milliseconds timeout) {
    auto enc = dvi::encode_request(op, payload);
    if (!enc.has_value())
        return failure(enc.error());
    if (auto s = chan.write_bytes(*enc); !s.has_value()) {
        return failure(s.error());
    }
    auto resp = read_dvi_frame(chan, timeout);
    if (!resp.has_value())
        return failure(resp.error());
    if (auto const *ef = std::get_if<dvi::ErrorFrame>(&*resp)) {
        char buf[96];
        std::snprintf(buf, sizeof buf,
                      "obdx::dvi_exchange: device returned error 0x%02X "
                      "for request opcode 0x%02X",
                      static_cast<unsigned>(ef->error_code),
                      static_cast<unsigned>(ef->request_opcode));
        return failure(ErrorCode::TransportNack, std::string{buf});
    }
    if (auto const *rf = std::get_if<dvi::ResponseFrame>(&*resp)) {
        return rf->payload;
    }
    // Unreachable per the DVI decoder's variant shape (Response or
    // Error are the only alternatives). Belt-and-suspenders for the
    // null-deref analyzer.
    return failure(ErrorCode::Unknown, "obdx::dvi_exchange: decoded frame matched no known "
                                       "variant");
}

// Map a LinkConfig to the DVI SetProtocol data payload, per the VT v1.06
// Reference Guide §3.10.1 ("Setting and Requesting the OBD Protocol").
//
// The full SetProtocol frame on the wire is:
//   CMD=0x31  LEN=0x02  SUB=0x01  PROTO=XX  CHK
// where PROTO is:
//   0x00 ALDL  0x01 VPW  0x02 HS CAN  0x03 MS CAN  0x04 GMLAN
//   0x05 PWM (not implemented)  0x06 LIN (not implemented)
//
// Only the data payload (SUB + PROTO, 2 bytes) is returned here; the
// outer CMD + LEN + CHK frame is built by `encode_request` in the DVI
// codec layer.
//
// Note that SetProtocol carries NEITHER the bus baud rate NOR the CAN
// IDs — baud is implicit per protocol (HS CAN = 500 kbps). CAN IDs
// (request + response) are configured separately via the 0x34 CAN
// Protocol Settings family (Developers Reference Manual §3.14), which
// open() drives right after SetProtocol. `LinkConfig::baud` is still
// unused on OBDX (the silicon doesn't accept off-spec rates), but
// `can_id_request` / `can_id_response` ARE consumed — they drive the
// filter setup so the adapter knows where to send requests and what
// to listen for on response.
//
// Diagnosed on real hardware 2026-05-22: an earlier 11-byte best-
// guess shape returned OBDX error 0x05 (Error_SubCommandIncorrectSize)
// with protocol enum 0x06 (LIN, not even implemented). Both fixed by
// matching the documented 2-byte format.
//
// Returns a 2-byte payload or InvalidArgument when LinkConfig requests
// a protocol the VX can't do.
[[nodiscard]] Result<std::vector<std::uint8_t>> set_protocol_payload(LinkConfig const &cfg) {
    constexpr std::uint8_t kSubSetProtocol = 0x01;
    constexpr std::uint8_t kProtoHsCan = 0x02;

    switch (cfg.kind) {
    case LinkKind::KLine:
        return failure(ErrorCode::InvalidArgument,
                       "obdx::Transport: OBDX VX doesn't support K-Line / ISO9141. "
                       "Only pre-2008 Subarus need K-Line; 2008+ (including VA/VB WRX) "
                       "run CAN-ISO15765 — use LinkKind::CanIso15765. If you DO need "
                       "K-Line for an older EJ Subaru, a Tactrix OpenPort is the usual "
                       "choice. (OBDX VX supported protocols per VT v1.06 §3.10.1: ALDL, "
                       "VPW, HS CAN, MS CAN, GMLAN.)");
    case LinkKind::CanFd:
        return failure(ErrorCode::InvalidArgument, "obdx::Transport: OBDX VX doesn't support "
                                                   "CAN-FD (the STN2120 silicon is classical-"
                                                   "CAN-only).");
    case LinkKind::CanIso15765:
        return std::vector<std::uint8_t>{kSubSetProtocol, kProtoHsCan};
    }
    return failure(ErrorCode::InvalidArgument, "obdx::Transport: unknown LinkKind");
}

// Build the DVI payload that enables network communication on the
// currently-selected protocol, per VT v1.06 §3.10.2.
//   Wire frame: CMD=0x31  LEN=0x02  SUB=0x02  STATE=0x01  CHK
// where STATE is 0x00 OFF / 0x01 ON / 0x02 LISTEN ONLY. Default is
// OFF — without this step the adapter accepts SetProtocol but won't
// actually pass bytes to the bus, so opening the link looks fine
// but every send_recv() times out. `state_on=false` selects LISTEN-
// ONLY mode for sniff sessions where another tool (e.g. COBB AP via
// a Y-cable) is bus-mastering and any TX from us would collide.
[[nodiscard]] std::vector<std::uint8_t> enable_network_payload(bool state_on = true) {
    constexpr std::uint8_t kSubEnableNetwork = 0x02;
    constexpr std::uint8_t kStateOn = 0x01;
    constexpr std::uint8_t kStateListenOnly = 0x02;
    return {kSubEnableNetwork, state_on ? kStateOn : kStateListenOnly};
}

// Build an OBDX "Entire Filter" payload (Developers Reference Manual
// v3.00 §3.14.1). Wire frame:
//   CMD=0x34  LEN=0x11(17)  SUB=0x00  MM  NN  XX  ZZ  <4B FilterID>
//                                            <4B Mask>  <4B FlowID>  CHK
// where:
//   SUB=0x00 = Entire Filter (set every field in one shot)
//   MM       = filter slot index (0..27 for 11-bit, default capacity)
//   NN       = frame type 0x00 = 11-bit, 0x01 = 29-bit
//   XX       = filter type 0x00 = Pass, 0x01 = Flow, 0x02 = Block
//   ZZ       = status 0x00 = off, 0x01 = on
//   Filter ID / Mask = CAN-ID we want to receive (big-endian)
//   Flow ID  = CAN-ID the adapter sends ISO-TP Flow Control on
//
// Without this command, the adapter has no filter configured for the
// ECU's response ID. Even though §3.4 says "the network is turned off
// so no frames are received and all filters are set to off", it leaves
// you a chicken-and-egg: enabling the network without filters means
// every ECU response is silently dropped → every send_recv times out
// in RX phase. Configuring one Flow filter (Filter ID = ECU response,
// Flow ID = our request) fixes that AND lets the adapter handle
// ISO-TP flow control on multi-frame outbound transfers automatically.
[[nodiscard]] std::vector<std::uint8_t>
can_filter_payload(std::uint32_t filter_id, std::uint32_t mask, std::uint32_t flow_id) {
    constexpr std::uint8_t kSubEntireFilter = 0x00;
    constexpr std::uint8_t kSlotZero = 0x00;
    constexpr std::uint8_t kFrameType11Bit = 0x00;
    constexpr std::uint8_t kFrameType29Bit = 0x01;
    constexpr std::uint8_t kFilterTypeFlow = 0x01;
    constexpr std::uint8_t kStatusOn = 0x01;

    // 29-bit IDs use any value above the 11-bit ceiling.
    bool const is_extended = filter_id > 0x7FFU || mask > 0x7FFU || flow_id > 0x7FFU;
    std::uint8_t const frame_type = is_extended ? kFrameType29Bit : kFrameType11Bit;

    // Same GCC 15 false-positive avoidance as send_recv/send: build the
    // 17-byte payload in a fixed-size array, then move into vector. No
    // reserve + push_back loops to trip `-Werror=free-nonheap-object`.
    std::uint8_t const bytes[17] = {
        kSubEntireFilter,
        kSlotZero,
        frame_type,
        kFilterTypeFlow,
        kStatusOn,
        static_cast<std::uint8_t>((filter_id >> 24U) & 0xFFU),
        static_cast<std::uint8_t>((filter_id >> 16U) & 0xFFU),
        static_cast<std::uint8_t>((filter_id >> 8U) & 0xFFU),
        static_cast<std::uint8_t>(filter_id & 0xFFU),
        static_cast<std::uint8_t>((mask >> 24U) & 0xFFU),
        static_cast<std::uint8_t>((mask >> 16U) & 0xFFU),
        static_cast<std::uint8_t>((mask >> 8U) & 0xFFU),
        static_cast<std::uint8_t>(mask & 0xFFU),
        static_cast<std::uint8_t>((flow_id >> 24U) & 0xFFU),
        static_cast<std::uint8_t>((flow_id >> 16U) & 0xFFU),
        static_cast<std::uint8_t>((flow_id >> 8U) & 0xFFU),
        static_cast<std::uint8_t>(flow_id & 0xFFU),
    };
    return std::vector<std::uint8_t>(std::begin(bytes), std::end(bytes));
}

// Strip the leading 4-byte big-endian CAN ID prefix that the OBDX
// includes in every CAN RxSmall push (Developers Reference Manual
// v3.00 §3.4.3). Returns the rest of the payload as a span and writes
// the recovered CAN ID into `out_id`. Fails if the payload is shorter
// than 4 bytes (which would be malformed for any CAN frame).
[[nodiscard]] Result<std::span<std::uint8_t const>>
strip_can_id(std::span<std::uint8_t const> payload, std::uint32_t &out_id) {
    if (payload.size() < 4) {
        return failure(ErrorCode::ParseError, "obdx::strip_can_id: RxSmall payload < 4 bytes — "
                                              "no room for CAN ID prefix");
    }
    out_id = (static_cast<std::uint32_t>(payload[0]) << 24U) |
             (static_cast<std::uint32_t>(payload[1]) << 16U) |
             (static_cast<std::uint32_t>(payload[2]) << 8U) |
             static_cast<std::uint32_t>(payload[3]);
    return payload.subspan(4);
}

// Strip the ISO-TP PCI byte(s) from a CAN frame's data field. Per ISO
// 15765-2, the first nibble identifies the frame type:
//   0x0L  Single Frame, L = number of data bytes (1..7).  Strip 1 byte.
//   0x1X YY  First Frame of a multi-frame message; 12-bit length
//            spread across the low nibble + next byte.  Strip 2 bytes.
//            (Assumes the adapter reassembled the full payload behind
//            the FF header — observed behavior on OBDX with auto
//            formatting + Flow filter; if this turns out wrong on
//            hardware we'll surface multi-frame as a distinct path.)
//   0x2X     Consecutive Frame — should never reach the application
//            layer when a Flow filter is active (the adapter handles
//            reassembly). Return as-is and let the protocol parser
//            barf with a clear message.
//   0x3X     Flow Control from us → never an inbound payload on a
//            real ECU read.  Same handling as 0x2X.
//
// IMPLICIT ADAPTER-DEFAULT DEPENDENCY (logged 2026-05-24 from analyst-
// side decompile of OBDXVX_J2534.dll): the OBDX J2534 driver exposes
// an `AutoProcessAndBulkRead` toggle (1 = adapter reassembles ISO-TP,
// 0 = host receives raw CAN frames). Our DVI-mode open() never
// explicitly sets the equivalent in the native binary protocol — we
// rely on the adapter's power-on default doing the right thing. This
// has worked empirically across every UDS / SSM test against the
// 2017 LF79103P reference hardware, but a future firmware version or
// a different OBDX model could ship a different default and silently
// break multi-frame reads. If the FF / CF assumptions above ever fail
// in production, this is the first place to look — explicit
// configuration via DVI is the principled fix.
[[nodiscard]] std::span<std::uint8_t const>
strip_iso_tp(std::span<std::uint8_t const> data) noexcept {
    if (data.empty()) {
        return data;
    }
    std::uint8_t const pci_hi = static_cast<std::uint8_t>(data[0] & 0xF0U);
    if (pci_hi == 0x00U) {
        std::size_t const len = data[0] & 0x0FU;
        std::size_t const take = std::min<std::size_t>(len, data.size() - 1);
        return data.subspan(1, take);
    }
    if (pci_hi == 0x10U) {
        if (data.size() < 2) {
            return data;
        }
        std::size_t const len = (static_cast<std::size_t>(data[0] & 0x0FU) << 8U) |
                                static_cast<std::size_t>(data[1]);
        std::size_t const take = std::min<std::size_t>(len, data.size() - 2);
        return data.subspan(2, take);
    }
    return data;
}

// Sniff an ELM-mode response for a string that suggests we're
// talking to an actual OBDX device. The probe response varies
// across firmware revisions; this is a loose-and-permissive check
// (anything mentioning "OBDX" passes). Tightening lands when we
// have a real adapter's response to pattern-match against.
[[nodiscard]] bool looks_like_obdx(std::string_view probe) noexcept {
    auto const has = [probe](std::string_view needle) {
        return probe.find(needle) != std::string_view::npos;
    };
    return has("OBDX") || has("obdx");
}

constexpr milliseconds kElmProbeTimeout{500};
constexpr milliseconds kDviSwitchTimeout{500};
constexpr milliseconds kSetProtocolTimeout{500};
constexpr milliseconds kCanFilterTimeout{500};
constexpr milliseconds kCloseTimeout{200};

} // namespace

void set_trace_enabled(bool on) noexcept {
    g_trace_enabled.store(on, std::memory_order_release);
}

bool trace_enabled() noexcept {
    return g_trace_enabled.load(std::memory_order_acquire);
}

Transport::Transport(std::unique_ptr<IDeviceChannel> channel) noexcept
    : channel_(std::move(channel)) {}

Transport::~Transport() {
    // Stop the streaming thread first — it holds the byte channel
    // and will deadlock close() if still running.
    (void)stop_streaming();
    if (open_) {
        (void)close();
    }
}

st::Status Transport::open(LinkConfig const &cfg) {
    if (open_) {
        return failure(ErrorCode::InvalidArgument,
                       "obdx::Transport::open: already open — close() first");
    }
    if (channel_ == nullptr) {
        return failure(ErrorCode::TransportUnavailable,
                       "obdx::Transport::open: no byte channel attached");
    }

    bool const trace = g_trace_enabled.load(std::memory_order_acquire);
    if (trace) {
        std::fprintf(stderr, "[trace][obdx-open] step 1: ELM probe (AT @1)\n");
        std::fflush(stderr);
    }

    // 1. ELM-mode probe. `AT @1` returns the device's identifier
    // string; we verify it mentions OBDX before sending anything
    // that could disturb a non-OBDX device on the same port.
    auto probe = elm_exchange(*channel_, "AT @1", kElmProbeTimeout);
    if (!probe.has_value()) {
        if (trace) {
            std::fprintf(stderr, "[trace][obdx-open] step 1 FAILED: %s\n",
                         std::string{probe.error().message()}.c_str());
            std::fflush(stderr);
        }
        return failure(probe.error());
    }
    if (trace) {
        std::fprintf(stderr, "[trace][obdx-open] step 1 OK: '%s'\n", probe->c_str());
        std::fflush(stderr);
    }
    if (!looks_like_obdx(*probe)) {
        std::string msg{"obdx::Transport::open: device did not identify as OBDX "
                        "(probe response: '"};
        msg.append(*probe);
        msg.append("'). Wrong adapter on this port?");
        return failure(ErrorCode::TransportUnavailable, std::move(msg));
    }
    firmware_ = *probe;

    if (trace) {
        std::fprintf(stderr, "[trace][obdx-open] step 2: switch to DVI (DX DP 1)\n");
        std::fflush(stderr);
    }
    // 2. Switch to DVI binary mode. Last ASCII exchange — after
    // this the adapter speaks DVI on the wire.
    auto switch_rsp = elm_exchange(*channel_, "DX DP 1", kDviSwitchTimeout);
    if (!switch_rsp.has_value()) {
        if (trace) {
            std::fprintf(stderr, "[trace][obdx-open] step 2 FAILED: %s\n",
                         std::string{switch_rsp.error().message()}.c_str());
            std::fflush(stderr);
        }
        return failure(switch_rsp.error());
    }
    if (trace) {
        std::fprintf(stderr, "[trace][obdx-open] step 2 OK: '%s'\n", switch_rsp->c_str());
        std::fflush(stderr);
    }

    // 3. Configure the bus per LinkConfig via DVI SetProtocol
    // (§3.10.1: sub-command 0x01, picks the OBD protocol enum).
    auto sp = set_protocol_payload(cfg);
    if (!sp.has_value()) {
        return failure(sp.error());
    }
    if (trace) {
        trace_dump("[trace][obdx-open] step 3 TX SetProtocol", *sp);
    }
    auto setp = dvi_exchange(*channel_, dvi::Opcode::SetProtocol, *sp, kSetProtocolTimeout);
    if (!setp.has_value()) {
        if (trace) {
            std::fprintf(stderr, "[trace][obdx-open] step 3 FAILED: %s\n",
                         std::string{setp.error().message()}.c_str());
            std::fflush(stderr);
        }
        return failure(setp.error());
    }
    if (trace) {
        trace_dump("[trace][obdx-open] step 3 RX SetProtocol ACK", *setp);
    }

    // 4. Configure a CAN Flow filter so the adapter knows which CAN ID
    // is the ECU's response (and where to send ISO-TP Flow Control).
    // Per Developers Reference Manual v3.14: "It is recommended to set
    // at least one filter before enabling the network to prevent
    // unwanted frames." Without this the adapter accepts EnableNetwork
    // and SetProtocol but silently drops every incoming ECU response —
    // exactly the silent-timeout failure mode diagnosed against the
    // 2017 WRX on 2026-05-23. Mask 0x7FF for 11-bit (Subaru standard)
    // means "match all 11 bits exactly". Only emitted for CAN; K-Line
    // / CAN-FD are rejected above, so reaching here implies CanIso15765.
    //
    // Sniff (listen_only) mode uses a PERMISSIVE filter (id=0, mask=0,
    // flow_id=0) rather than skipping the filter setup entirely. The
    // earlier "skip the filter for sniff" approach (per a misreading of
    // §3.4) hit the same silent-drop behavior diagnosed above: with no
    // filter configured, the adapter accepts EnableNetwork but pushes
    // ZERO frames to the host. Empirically confirmed on the user's 2017
    // VA WRX — a 142s bus-check captured 0 frames even with the engine
    // running and an AccessPort actively negotiating settings on the
    // bus. mask=0 means "(received_id & 0) == (filter_id & 0)" — always
    // true, so every frame matches and gets pushed.
    if (cfg.kind == LinkKind::CanIso15765) {
        constexpr std::uint32_t k11BitMask = 0x000007FFU;
        std::uint32_t const filt_id =
            cfg.listen_only ? 0x000U : cfg.can_id_response;
        std::uint32_t const filt_mask =
            cfg.listen_only ? 0x000U : k11BitMask;
        std::uint32_t const flow_id =
            cfg.listen_only ? 0x000U : cfg.can_id_request;
        auto const filt = can_filter_payload(filt_id, filt_mask, flow_id);
        if (trace) {
            trace_dump(cfg.listen_only
                           ? "[trace][obdx-open] step 4 TX CAN Filter (permissive, listen-only)"
                           : "[trace][obdx-open] step 4 TX CAN Filter setup",
                       filt);
        }
        auto filt_rsp =
            dvi_exchange(*channel_, dvi::Opcode::CanProtocolSettings, filt, kCanFilterTimeout);
        if (!filt_rsp.has_value()) {
            if (trace) {
                std::fprintf(stderr, "[trace][obdx-open] step 4 FAILED: %s\n",
                             std::string{filt_rsp.error().message()}.c_str());
                std::fflush(stderr);
            }
            return failure(filt_rsp.error());
        }
        if (trace) {
            trace_dump("[trace][obdx-open] step 4 RX CAN Filter ACK", *filt_rsp);
        }
    }

    // 5. Enable network communication (§3.10.2: same 0x31 opcode,
    // sub-command 0x02, state 0x01 ON / 0x02 LISTEN-ONLY). Default is
    // OFF — without this step send_recv would time out on every
    // exchange. In listen_only mode we use STATE=0x02 so the adapter
    // never transmits — critical when sharing the bus with another tool.
    auto const en = enable_network_payload(/*state_on=*/!cfg.listen_only);
    if (trace) {
        trace_dump("[trace][obdx-open] step 5 TX EnableNetwork", en);
    }
    auto enable_rsp = dvi_exchange(*channel_, dvi::Opcode::SetProtocol, en, kSetProtocolTimeout);
    if (!enable_rsp.has_value()) {
        if (trace) {
            std::fprintf(stderr, "[trace][obdx-open] step 5 FAILED: %s\n",
                         std::string{enable_rsp.error().message()}.c_str());
            std::fflush(stderr);
        }
        return failure(enable_rsp.error());
    }
    if (trace) {
        trace_dump("[trace][obdx-open] step 5 RX EnableNetwork ACK", *enable_rsp);
    }

    // EnableNetwork's positive response per VT v1.06 §3.10.2 echoes the
    // sub-command byte (0x02) followed by the final state byte (0x01 ON,
    // 0x02 LISTEN-ONLY, 0x00 OFF). If the adapter ACK'd the request but
    // the state byte isn't 0x01, the bus didn't actually come up — the
    // adapter accepted the command but couldn't bring the network online
    // (cable disconnected, ECU not powered, bus contention). Catching it
    // here means the caller gets a meaningful error at open() time
    // instead of a string of inscrutable send_recv timeouts later.
    //
    // The check is conservative on the response *shape*: anything shorter
    // than 2 bytes, sub-op echo other than 0x02, or state byte not in
    // {ON, LISTEN-ONLY} is rejected. LISTEN-ONLY is accepted with a
    // verbose warning because some firmware revs may report it when the
    // adapter chose listen-only after a contention probe.
    constexpr std::uint8_t kEnableSubOp = 0x02;
    constexpr std::uint8_t kStateOn = 0x01;
    constexpr std::uint8_t kStateListenOnly = 0x02;

    // Validation-failure helper: the adapter has already been switched into
    // DVI mode + had a protocol set; bailing here without resetting it would
    // leave it stuck and the user's next open() attempt would fail
    // confusingly on the ELM probe (`AT @1` returns nothing in DVI mode).
    // Best-effort SoftReboot returns the adapter to ELM mode for the next
    // attempt. Ignore the result — the channel may be gone (USB unplugged),
    // which is fine for cleanup.
    auto const bail_from_validation = [&](st::Error err) {
        (void)dvi_exchange(*channel_, dvi::Opcode::SoftReboot,
                           std::span<std::uint8_t const>{}, kCloseTimeout);
        return failure(std::move(err));
    };

    if (enable_rsp->size() < 2) {
        return bail_from_validation(
            st::Error{ErrorCode::TransportNack,
                      "obdx::Transport::open: EnableNetwork response too "
                      "short — expected sub-op echo + state byte, got " +
                          std::to_string(enable_rsp->size()) + " bytes"});
    }
    if ((*enable_rsp)[0] != kEnableSubOp) {
        char buf[96];
        std::snprintf(buf, sizeof buf,
                      "obdx::Transport::open: EnableNetwork response sub-op "
                      "echo is 0x%02X, expected 0x02",
                      static_cast<unsigned>((*enable_rsp)[0]));
        return bail_from_validation(st::Error{ErrorCode::TransportNack, std::string{buf}});
    }
    auto const state_byte = (*enable_rsp)[1];
    if (state_byte != kStateOn && state_byte != kStateListenOnly) {
        char buf[160];
        std::snprintf(buf, sizeof buf,
                      "obdx::Transport::open: EnableNetwork acknowledged but "
                      "state byte 0x%02X is not ON (0x01) — the adapter could "
                      "not bring the bus online (cable / ignition / contention)",
                      static_cast<unsigned>(state_byte));
        return bail_from_validation(st::Error{ErrorCode::TransportNack, std::string{buf}});
    }
    // Warn only when LISTEN-ONLY was NOT requested — an unexpected
    // LISTEN-ONLY may indicate adapter chose listen-only after a
    // contention probe and our active sends won't reach the bus.
    // When listen_only=true, LISTEN-ONLY is the expected response and
    // not worth warning about.
    if (trace && state_byte == kStateListenOnly && !cfg.listen_only) {
        std::fprintf(stderr, "[trace][obdx-open] WARNING: EnableNetwork "
                             "returned LISTEN-ONLY (0x02) but caller asked for "
                             "active mode; send paths may not reach the bus.\n");
        std::fflush(stderr);
    }

    kind_ = cfg.kind;
    can_id_request_ = cfg.can_id_request;
    can_id_response_ = cfg.can_id_response;
    listen_only_ = cfg.listen_only;
    open_ = true;
    return ok();
}

st::Status Transport::close() {
    if (!open_) {
        return ok();
    }
    open_ = false; // mark closed up front so a failing reboot
                   // doesn't trap us in a re-close loop
    // Best-effort SoftReboot — returns the adapter to ELM mode
    // for the next consumer. Ignore the result; the channel may
    // already be gone (USB unplugged), which is fine for close().
    (void)dvi_exchange(*channel_, dvi::Opcode::SoftReboot, std::span<std::uint8_t const>{},
                       kCloseTimeout);
    firmware_.clear();
    return ok();
}

Result<Frame> Transport::send_recv(std::span<std::uint8_t const> payload,
                                   std::chrono::milliseconds timeout) {
    if (!open_) {
        return failure(ErrorCode::TransportUnavailable,
                       "obdx::Transport::send_recv: transport not open");
    }
    if (channel_ == nullptr) {
        return failure(ErrorCode::TransportUnavailable,
                       "obdx::Transport::send_recv: no byte channel");
    }
    if (listen_only_) {
        return failure(ErrorCode::TransportUnavailable,
                       "obdx::Transport::send_recv: transport opened with "
                       "listen_only=true (sniff mode); active sends are "
                       "disabled to avoid colliding with another tool on "
                       "the bus. Use start_streaming() to receive frames.");
    }

    // Split the host-side budget across the two DVI exchanges. For
    // small (single-frame ISO-TP) payloads the adapter ACKs the TX
    // almost immediately, so the historical 25/75 TX/RX split is
    // fine. For larger payloads the adapter has to do multi-frame
    // ISO-TP (send a First Frame, WAIT for the ECU's Flow Control,
    // then stream Consecutive Frames) and the TX-side ACK can take
    // hundreds of ms — at 500 kbps with even a modest STmin the
    // 35-frame transmission for an 80-address SSM A8 read can run
    // to 300+ ms. Use a 50/50 split when payload > 32 B (single-
    // frame ISO-TP threshold for OBD-II addressing is 7 B, so >32
    // is comfortably into multi-frame territory).
    bool const multi_frame_tx = payload.size() > 32;
    auto const tx_share = multi_frame_tx ? timeout.count() / 2 : timeout.count() / 4;
    auto const tx_budget = std::max(milliseconds{1}, milliseconds{tx_share});
    auto const rx_budget = std::max(milliseconds{1}, timeout - tx_budget);

    bool const trace = g_trace_enabled.load(std::memory_order_acquire);
    if (trace) {
        trace_dump("[trace][obdx-tx]", payload);
    }

    // Phase 1: TX the ECU payload onto the bus via DVI TxSmall.
    // CAN-ISO15765 TxSmall payload is `[4B BE CAN ID][user bytes]` per
    // Developers Reference Manual v3.00 §3.6.3. Without the CAN ID
    // prefix the adapter has no destination — bytes go nowhere and the
    // ECU never sees the request. Total wire payload is +4 vs the
    // user payload, so the 255 B limit needs the same +4 accounting.
    std::vector<std::uint8_t> tx_payload;
    if (kind_ == LinkKind::CanIso15765) {
        if (payload.size() + 4 > 255) {
            // TODO(transport_obdx): TxLarge (opcode 0x11, 2-byte length)
            // for payloads > 251 B. Not relevant for ITransport's normal
            // request shape (SSM/UDS payloads are well under 100 B) but
            // worth surfacing as a clean error until implemented.
            return failure(ErrorCode::InvalidArgument,
                           "obdx::Transport::send_recv: payload > 251 B (+ 4B CAN ID prefix > "
                           "255) needs DVI TxLarge — not yet wired");
        }
        // Build the prefix in a fixed-size array first, then assign +
        // insert. GCC 15 raises a false-positive `-Werror=free-nonheap-
        // object` on the reserve-then-push_back pattern with small
        // capacities (see prior fix in this file's git history); the
        // assign+insert shape sidesteps it without losing the single
        // allocation.
        std::uint8_t const prefix[4] = {
            static_cast<std::uint8_t>((can_id_request_ >> 24U) & 0xFFU),
            static_cast<std::uint8_t>((can_id_request_ >> 16U) & 0xFFU),
            static_cast<std::uint8_t>((can_id_request_ >> 8U) & 0xFFU),
            static_cast<std::uint8_t>(can_id_request_ & 0xFFU),
        };
        tx_payload.assign(std::begin(prefix), std::end(prefix));
        tx_payload.insert(tx_payload.end(), payload.begin(), payload.end());
    } else {
        if (payload.size() > 255) {
            return failure(ErrorCode::InvalidArgument,
                           "obdx::Transport::send_recv: payload > 255 B needs DVI TxLarge — "
                           "not yet wired");
        }
        tx_payload.assign(payload.begin(), payload.end());
    }
    auto tx = dvi_exchange(*channel_, dvi::Opcode::TxSmall, tx_payload, tx_budget);
    if (!tx.has_value()) {
        if (trace) {
            std::fprintf(stderr, "[trace][obdx-err] TX phase: %s\n",
                         std::string{tx.error().message()}.c_str());
            std::fflush(stderr);
        }
        return failure(tx.error());
    }

    // Phase 2: read the ECU's response. Critical distinction per VT
    // v1.06 §3.3: RxSmall (opcode 0x08) is an ADAPTER-TO-PC push only
    // — the OBDX automatically pushes a `08 LEN <frame> CHK` frame
    // whenever a network message arrives. The PC does NOT request it
    // with opcode 0x08 (that's Error_InvalidCommand 0x01 — diagnosed
    // 2026-05-22 on a real WRX).
    //
    // So this phase passively reads the next frame off the channel,
    // expecting an unsolicited 0x08 (RxSmall) frame from the adapter.
    // 0x09 (RxLarge) is the wider variant for frames > 255 B; we
    // surface anything else as a protocol error rather than silently
    // accept e.g. another error frame.
    auto resp = read_dvi_frame(*channel_, rx_budget);
    if (!resp.has_value()) {
        if (trace) {
            std::fprintf(stderr, "[trace][obdx-err] RX phase: %s\n",
                         std::string{resp.error().message()}.c_str());
            std::fflush(stderr);
        }
        return failure(resp.error());
    }
    if (auto const *ef = std::get_if<dvi::ErrorFrame>(&*resp)) {
        char buf[96];
        std::snprintf(buf, sizeof buf,
                      "obdx::send_recv: device returned error 0x%02X "
                      "for opcode 0x%02X while awaiting ECU response",
                      static_cast<unsigned>(ef->error_code),
                      static_cast<unsigned>(ef->request_opcode));
        return failure(ErrorCode::TransportNack, std::string{buf});
    }
    auto const *rf = std::get_if<dvi::ResponseFrame>(&*resp);
    if (rf == nullptr) {
        return failure(ErrorCode::Unknown,
                       "obdx::send_recv: decoded frame matched no known variant");
    }
    // Per spec, adapter→PC pushes use the bare opcode (NOT |0x10) —
    // so we expect 0x08 for RxSmall, not 0x18.
    auto const expected_rx = static_cast<std::uint8_t>(dvi::Opcode::RxSmall);
    auto const expected_rx_large = static_cast<std::uint8_t>(dvi::Opcode::RxLarge);
    if (rf->response_opcode != expected_rx && rf->response_opcode != expected_rx_large) {
        char buf[96];
        std::snprintf(buf, sizeof buf,
                      "obdx::send_recv: expected unsolicited RxSmall/Large (0x08/0x09); "
                      "got opcode 0x%02X",
                      static_cast<unsigned>(rf->response_opcode));
        return failure(ErrorCode::TransportNack, std::string{buf});
    }
    // CAN-ISO15765 RxSmall payload is `[4B BE CAN ID][ISO-TP PCI...][app bytes]`
    // per Developers Reference Manual v3.00 §3.4.3. Strip both prefixes so
    // the caller (SSM / UDS client) sees only application-layer bytes,
    // exactly as the existing test fixtures and protocol parsers expect.
    Frame f;
    f.arrived = steady_clock::now();
    if (kind_ == LinkKind::CanIso15765) {
        std::uint32_t rx_can_id = 0;
        auto stripped = strip_can_id(rf->payload, rx_can_id);
        if (!stripped.has_value()) {
            return failure(stripped.error());
        }
        if (rx_can_id != can_id_response_) {
            char buf[128];
            std::snprintf(buf, sizeof buf,
                          "obdx::send_recv: ECU response arrived on CAN ID 0x%X but expected "
                          "0x%X — wrong module or stray bus traffic",
                          rx_can_id, can_id_response_);
            return failure(ErrorCode::TransportNack, std::string{buf});
        }
        auto const app = strip_iso_tp(*stripped);
        f.data.assign(app.begin(), app.end());
    } else {
        f.data = std::move(rf->payload);
    }
    if (trace) {
        trace_dump("[trace][obdx-rx]", f.data);
    }
    return f;
}

st::Status Transport::send(std::span<std::uint8_t const> payload) {
    if (!open_) {
        return failure(ErrorCode::TransportUnavailable,
                       "obdx::Transport::send: transport not open");
    }
    if (channel_ == nullptr) {
        return failure(ErrorCode::TransportUnavailable, "obdx::Transport::send: no byte channel");
    }
    if (listen_only_) {
        return failure(ErrorCode::TransportUnavailable,
                       "obdx::Transport::send: transport opened with "
                       "listen_only=true (sniff mode); active sends are "
                       "disabled");
    }
    // Same +4B CAN ID prefix dance as send_recv: the wire format isn't
    // protocol-aware, so fire-and-forget sends still need the destination
    // ID prepended on CAN.
    std::vector<std::uint8_t> tx_payload;
    if (kind_ == LinkKind::CanIso15765) {
        if (payload.size() + 4 > 255) {
            return failure(ErrorCode::InvalidArgument,
                           "obdx::Transport::send: payload > 251 B (+ 4B CAN ID prefix > 255) "
                           "needs DVI TxLarge — not yet wired");
        }
        std::uint8_t const prefix[4] = {
            static_cast<std::uint8_t>((can_id_request_ >> 24U) & 0xFFU),
            static_cast<std::uint8_t>((can_id_request_ >> 16U) & 0xFFU),
            static_cast<std::uint8_t>((can_id_request_ >> 8U) & 0xFFU),
            static_cast<std::uint8_t>(can_id_request_ & 0xFFU),
        };
        tx_payload.assign(std::begin(prefix), std::end(prefix));
        tx_payload.insert(tx_payload.end(), payload.begin(), payload.end());
    } else {
        if (payload.size() > 255) {
            return failure(ErrorCode::InvalidArgument, "obdx::Transport::send: payload > 255 B "
                                                       "needs DVI TxLarge — not yet wired");
        }
        tx_payload.assign(payload.begin(), payload.end());
    }
    auto tx = dvi_exchange(*channel_, dvi::Opcode::TxSmall, tx_payload, milliseconds{100});
    if (!tx.has_value())
        return failure(tx.error());
    return ok();
}

st::Status Transport::start_streaming(FrameCallback callback) {
    if (!open_) {
        return failure(ErrorCode::TransportUnavailable,
                       "obdx::Transport::start_streaming: transport not open");
    }
    if (channel_ == nullptr) {
        return failure(ErrorCode::TransportUnavailable,
                       "obdx::Transport::start_streaming: no byte channel");
    }
    if (streaming_thread_.joinable()) {
        return failure(ErrorCode::InvalidArgument,
                       "obdx::Transport::start_streaming: already streaming — "
                       "call stop_streaming() first");
    }
    if (!callback) {
        return failure(ErrorCode::InvalidArgument,
                       "obdx::Transport::start_streaming: callback is empty");
    }

    bool const trace = g_trace_enabled.load(std::memory_order_acquire);
    if (trace) {
        std::fprintf(stderr,
                     "[trace][obdx-stream] starting (listen_only=%d)\n",
                     listen_only_ ? 1 : 0);
        std::fflush(stderr);
    }

    streaming_callback_ = std::move(callback);
    streaming_stop_.store(false, std::memory_order_release);

    streaming_thread_ = std::thread([this]() {
        bool const trace_local = g_trace_enabled.load(std::memory_order_acquire);
        // Short per-read timeout so the thread re-checks the stop flag
        // promptly. A bus that's busy with COBB+ECU traffic produces
        // many frames per second; sleeping on a long timeout would
        // make stop_streaming sluggish.
        constexpr milliseconds kStreamReadTimeout{50};
        while (!streaming_stop_.load(std::memory_order_acquire)) {
            auto frame = read_dvi_frame(*channel_, kStreamReadTimeout);
            if (!frame.has_value()) {
                // TransportTimeout is the empty-bus case — just loop.
                // Anything else is a real error; log under trace but
                // don't terminate (the bus might recover).
                if (trace_local &&
                    frame.error().code() != ErrorCode::TransportTimeout) {
                    std::fprintf(stderr,
                                 "[trace][obdx-stream-err] %s\n",
                                 std::string{frame.error().message()}.c_str());
                    std::fflush(stderr);
                }
                continue;
            }
            auto const *rf = std::get_if<dvi::ResponseFrame>(&*frame);
            if (rf == nullptr) {
                // ErrorFrame — log and skip; not fatal in sniff mode.
                if (trace_local) {
                    std::fprintf(stderr,
                                 "[trace][obdx-stream] ignoring error frame\n");
                    std::fflush(stderr);
                }
                continue;
            }
            // Per VT v1.06 §3.3 only the bare 0x08/0x09 opcodes are
            // adapter→PC pushes (network arrivals). Anything else is
            // an unsolicited response we have no business processing
            // while streaming.
            auto const op = rf->response_opcode;
            if (op != static_cast<std::uint8_t>(dvi::Opcode::RxSmall) &&
                op != static_cast<std::uint8_t>(dvi::Opcode::RxLarge)) {
                if (trace_local) {
                    std::fprintf(stderr,
                                 "[trace][obdx-stream] ignoring opcode 0x%02X "
                                 "(not RxSmall/RxLarge)\n",
                                 static_cast<unsigned>(op));
                    std::fflush(stderr);
                }
                continue;
            }

            Frame out;
            out.arrived = steady_clock::now();
            if (kind_ == LinkKind::CanIso15765) {
                std::uint32_t rx_can_id = 0;
                auto stripped = strip_can_id(rf->payload, rx_can_id);
                if (!stripped.has_value()) {
                    if (trace_local) {
                        std::fprintf(stderr,
                                     "[trace][obdx-stream] dropped short frame "
                                     "(< 4 B CAN ID prefix)\n");
                        std::fflush(stderr);
                    }
                    continue;
                }
                out.can_id = rx_can_id;
                // CRITICAL: do NOT strip the ISO-TP PCI byte in sniff
                // mode. The PCI byte is part of the raw bus frame and
                // the user is here to see real bus traffic byte-for-
                // byte. Stripping it would hide ISO-TP behavior the
                // user explicitly wants to observe (and would break
                // SecurityAccess capture — the seed bytes follow the
                // PCI byte directly).
                out.data.assign(stripped->begin(), stripped->end());
            } else {
                out.data = std::move(rf->payload);
            }
            if (trace_local) {
                std::fprintf(stderr,
                             "[trace][obdx-stream-rx] 0x%X (%zu B)\n",
                             out.can_id, out.data.size());
                std::fflush(stderr);
            }
            // The callback owns its own error handling; if it throws,
            // the streaming thread terminates rather than corrupting
            // adjacent frames. Document that contract at the call site
            // in start_streaming's docstring (already in transport.hpp).
            streaming_callback_(out);
        }
        if (trace_local) {
            std::fprintf(stderr, "[trace][obdx-stream] stopped\n");
            std::fflush(stderr);
        }
    });

    return ok();
}

st::Status Transport::stop_streaming() {
    if (!streaming_thread_.joinable()) {
        return ok();
    }
    streaming_stop_.store(true, std::memory_order_release);
    streaming_thread_.join();
    streaming_callback_ = {};
    return ok();
}

} // namespace st::transport::obdx
