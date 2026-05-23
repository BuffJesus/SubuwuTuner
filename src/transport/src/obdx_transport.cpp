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
// IDs — baud is implicit per protocol (HS CAN = 500 kbps), and the
// adapter handles OBD-II standard addressing (0x7E0 / 0x7E8) by
// default when in HS CAN mode. Per-bus filters (for non-standard
// addressing or extended-ID CAN) ride on the 0x33 family of commands.
// `LinkConfig::baud` / `LinkConfig::can_id_request` / `can_id_response`
// are kept for compatibility with other transport backends (J2534,
// native handheld); they're unused on OBDX.
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
// but every send_recv() times out.
[[nodiscard]] std::vector<std::uint8_t> enable_network_payload() {
    constexpr std::uint8_t kSubEnableNetwork = 0x02;
    constexpr std::uint8_t kStateOn = 0x01;
    return {kSubEnableNetwork, kStateOn};
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

    // 4. Enable network communication (§3.10.2: same 0x31 opcode,
    // sub-command 0x02, state 0x01 ON). Default is OFF — without
    // this step send_recv would time out on every exchange.
    auto const en = enable_network_payload();
    if (trace) {
        trace_dump("[trace][obdx-open] step 4 TX EnableNetwork", en);
    }
    auto enable_rsp = dvi_exchange(*channel_, dvi::Opcode::SetProtocol, en, kSetProtocolTimeout);
    if (!enable_rsp.has_value()) {
        if (trace) {
            std::fprintf(stderr, "[trace][obdx-open] step 4 FAILED: %s\n",
                         std::string{enable_rsp.error().message()}.c_str());
            std::fflush(stderr);
        }
        return failure(enable_rsp.error());
    }
    if (trace) {
        trace_dump("[trace][obdx-open] step 4 RX EnableNetwork ACK", *enable_rsp);
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
    if (trace && state_byte == kStateListenOnly) {
        std::fprintf(stderr, "[trace][obdx-open] WARNING: EnableNetwork "
                             "returned LISTEN-ONLY (0x02); send paths may not "
                             "reach the bus.\n");
        std::fflush(stderr);
    }

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
    if (payload.size() > 255) {
        // TODO(transport_obdx): TxLarge (opcode 0x11, 2-byte length)
        // for payloads > 255 B. Not relevant for ITransport's normal
        // request shape (SSM/UDS payloads are well under 100 B) but
        // worth surfacing as a clean error until implemented.
        return failure(ErrorCode::InvalidArgument, "obdx::Transport::send_recv: payload > 255 B "
                                                   "needs DVI TxLarge — not yet wired");
    }
    auto tx = dvi_exchange(*channel_, dvi::Opcode::TxSmall, payload, tx_budget);
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
    Frame f;
    f.data = std::move(rf->payload);
    f.arrived = steady_clock::now();
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
    if (payload.size() > 255) {
        return failure(ErrorCode::InvalidArgument, "obdx::Transport::send: payload > 255 B "
                                                   "needs DVI TxLarge — not yet wired");
    }
    auto tx = dvi_exchange(*channel_, dvi::Opcode::TxSmall, payload, milliseconds{100});
    if (!tx.has_value())
        return failure(tx.error());
    return ok();
}

st::Status Transport::start_streaming(FrameCallback /*callback*/) {
    return failure(ErrorCode::NotImplemented, "obdx::Transport::start_streaming: streaming lands "
                                              "with the datalogger I/O thread + ring buffer "
                                              "(Phase 3 follow-up).");
}

st::Status Transport::stop_streaming() {
    return failure(ErrorCode::NotImplemented,
                   "obdx::Transport::stop_streaming: see start_streaming");
}

} // namespace st::transport::obdx
