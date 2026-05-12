# 13 — Transport & ECU Protocol Design

This document captures the design for Phase 3 (comms and datalogging) **before any of it is implemented**. The Phase 2 calibration side (ROM I/O, definitions, editing, projects) is complete; everything below describes how SubuwuTuner will eventually talk to a real ECU through a real adapter.

Writing this now serves two purposes: (1) when the developer's OBDX Pro VX adapter arrives, implementation has a clear target, and (2) it ensures the calibration code we've already shipped is compatible with the transport layer's needs.

## Goals

- One **abstract transport** that the rest of the codebase talks to. Adapter-specific code lives behind it.
- Support, in this priority order, **Tactrix OpenPort 2.0 (J2534)** → **OBDX Pro VX** → **OBDLink (STN)** → **ELM327** (read-only).
- Support both Subaru protocol variants: **SSM** (legacy K-Line / KWP2000 on VA, also CAN-encapsulated on newer ECUs) and **UDS / KWP-on-CAN** (modern, used by VB).
- Be **scriptable**: every transport operation is reachable from `subuwutuner-cli` so logging, reads, and (eventually) writes can be driven from shell scripts and CI.
- Be **testable without hardware**: a `MockTransport` replays canned traces so SSM/UDS clients can be exercised in unit tests on every CI run.
- Sustain **100 Hz datalogging on supported adapters** (per `docs/05-improvements.md`) with a lock-free producer/consumer ring buffer between the I/O thread and the UI/CSV sink.

## Non-goals

- No "auto-detect adapter" magic in v1.x. The user picks an adapter from a list; the tool reports clearly if it can't find it.
- No transport over the network (Wi-Fi/BT adapters route to a virtual COM port). Networked tuning is a future concern.
- No adapter-write support for ELM327 — it's been the source of every classic-ELM bricking story; we exclude it on principle. Datalogging only.

## Module map (mirrors `docs/02-architecture.md`)

```
src/
├── transport/                 (st::transport — adapter-agnostic interface)
│   ├── include/st/transport.hpp
│   └── src/
├── transport_j2534/           (st::transport::j2534 — Windows DLL / Mac+Linux runtime)
├── transport_elm/             (st::transport::elm   — serial AT-command)
├── transport_stn/             (st::transport::stn   — OBDLink ST/STN extensions)
├── transport_obdx/            (st::transport::obdx  — OBDX Pro VX)
├── ecu/
│   ├── ssm/                   (st::ecu::ssm — Subaru Select Monitor protocol)
│   └── uds/                   (st::ecu::uds — ISO 14229)
└── log/                       (st::log — LogStream, ring buffer, CSV/FlatBuffers sinks)
```

The `st::transport` interface depends only on `st::core`; adapter implementations depend on `st::transport` plus their vendor SDK (or libusb / native serial for the open ones). `st::ecu::ssm` and `st::ecu::uds` depend on `st::transport` only, never on a specific adapter — this is what lets the same `SsmClient` talk through any compatible adapter.

## The ITransport interface

```cpp
namespace st::transport {

enum class LinkKind {
    KLine,            // ISO 9141 / 14230 (KWP2000) — VA WRX
    CanIso15765,      // CAN-TP — VB WRX and most modern Subarus
    CanFd,            // CAN-FD — future
};

struct LinkConfig {
    LinkKind kind;
    int      baud{};        // K-Line: 10400 typical; CAN: 500000 typical
    int      can_id_request{};  // for CAN only
    int      can_id_response{}; //   "
};

// One frame on the wire. Payload includes the application bytes only; the
// transport handles ISO-TP fragmentation/reassembly on CAN.
struct Frame {
    std::vector<std::uint8_t> data;
    std::chrono::steady_clock::time_point arrived;  // host-side timestamp
};

class ITransport {
public:
    virtual ~ITransport() = default;

    // Adapter-level open/close. open() configures the link per LinkConfig
    // and is the first call after construction; close() is idempotent.
    virtual Status open(LinkConfig const &cfg) = 0;
    virtual Status close() = 0;

    // Request/response with timeout. send_recv blocks up to `timeout` for
    // a reply matching the implicit response context (e.g. a specific
    // SSM responder address or UDS positive response).
    virtual Result<Frame> send_recv(std::span<std::uint8_t const> payload,
                                    std::chrono::milliseconds      timeout) = 0;

    // Fire-and-forget — used by some flashing routines that don't expect a
    // direct response. Returns immediately after the bytes leave the host.
    virtual Status send(std::span<std::uint8_t const> payload) = 0;

    // Continuous-receive mode for datalogging. The callback is invoked on
    // the transport's I/O thread; the implementation is responsible for
    // making it cheap and lock-free.
    using FrameCallback = std::function<void(Frame const &)>;
    virtual Status start_streaming(FrameCallback callback) = 0;
    virtual Status stop_streaming() = 0;

    // Adapter identity for logs and the UI's "connected device" indicator.
    virtual std::string_view name() const noexcept = 0;
    virtual std::string_view firmware() const noexcept = 0;
};

} // namespace st::transport
```

### Why a single `ITransport` and not separate read/write/stream interfaces?

Three reasons:

1. Adapters serialize commands at the device level — even if the API splits the interface, the device doesn't multiplex. Reflecting that in code prevents misuse.
2. Tests use one `MockTransport` and replay canned traces in any order — read, write, stream, switch protocol. Multiple interfaces would multiply the mocks for no value.
3. The dispatch surface stays small. Adapters implement six methods total.

## Adapter implementations

### J2534 (Tactrix OpenPort 2.0, Tactrix Pro J, others)

J2534 is an SAE-defined "pass-thru" API every commercial diagnostic adapter ships a driver for. On Windows the driver is a DLL registered under `HKLM\Software\PassThruSupport.04.04`; on macOS/Linux Tactrix's own runtime ships an equivalent shared library.

Implementation strategy:

- Dynamic-load the vendor DLL/SO at runtime — no compile-time vendor dependency, so the project builds without any vendor SDK present.
- Wrap the J2534 v04.04 entry points (`PassThruOpen`, `PassThruConnect`, `PassThruWriteMsgs`, `PassThruReadMsgs`, `PassThruIoctl`) behind our `ITransport`.
- Configure the link per `LinkConfig` via `PassThruIoctl(SET_CONFIG, ...)` for baud, parity, byte format.
- For SSM-over-K-Line, use protocol ID `ISO9141`; for SSM-over-CAN, `ISO15765`.

Risks: J2534 v04.04 vs v05.00 ABI differences (we target v04.04 because Tactrix's adapter implements that). Vendor DLLs vary in quality — we keep a per-adapter quirk table in `transport_j2534/quirks.cpp`.

### OBDX Pro VX (STN2120-based, native USB / Wi-Fi / BT)

OBDX exposes both an ELM-compatible AT command set and a richer native protocol. We use the native protocol for performance, falling back to AT only for adapter probing. Native protocol is well-documented by OBDX.

Implementation strategy:

- Open the USB CDC ACM endpoint via libusb (Windows + Linux) or the platform native serial driver (macOS, since Apple's I/O Kit handles CDC USB devices). BT and Wi-Fi route to virtual COM ports we treat identically.
- Send `STN` extension commands for CAN config (`STSBR` set baud, `STCFCP` configure CAN flow control protocol, `STMA` monitor all).
- ISO-TP fragmentation handled by the adapter when configured; we receive whole responses.

Risks: OBDX-specific firmware quirks aren't as widely tested in the community as Tactrix. We test against the developer's own adapter once it arrives and document any quirks discovered.

### OBDLink (STN-series — STN1110, STN2100, etc.)

Same approach as OBDX (they share the STN chip family). Different USB VID/PID and slightly different default config. We share most of `transport_obdx` with `transport_stn`.

### ELM327 (read-only, no write support)

Classic ELM327 is universal and cheap, but its firmware is firmware-and-a-half-decades old, has no native ISO-TP, and many clone implementations exist with subtle bugs. We support **datalogging only** — never reflashing or writing.

Implementation strategy:

- Pure AT command parsing.
- Manual ISO-TP fragmentation/reassembly on our side for CAN protocols.
- Maximum sample rate is ~20 Hz on most ELMs and we report that to the user.

## ECU protocol clients

### `st::ecu::ssm::SsmClient`

Implements Subaru Select Monitor over whatever link the transport provides.

```cpp
class SsmClient {
public:
    explicit SsmClient(ITransport &t);

    // SSM A8 read: reads N bytes from the ECU's address space (RAM or flash).
    Result<std::vector<std::uint8_t>> read(std::uint32_t address, std::size_t length,
                                            std::chrono::milliseconds timeout);

    // SSM B0 (or B8 on newer ECUs) write: writes N bytes. Returns the bytes
    // the ECU echoed, which is how we confirm the write took.
    Result<std::vector<std::uint8_t>> write(std::uint32_t address,
                                             std::span<std::uint8_t const> data,
                                             std::chrono::milliseconds timeout);

    // Seed/key authentication. The implementation is the public Subaru
    // algorithm; key derivation is per-ECU-family and lives in subclasses or
    // a function table keyed by ECU type.
    Result<void> authenticate(EcuFamily family);

    // Returns the ECU's responder identifier (used by some routines).
    Result<std::uint8_t> ecu_id();
};
```

Frame format (well-known):

```
Header   Dest    Source   Length  Command  Body                       Checksum
0x80     0x10    0xF0     0xNN    0xA8     [01] [addr_hi] [addr_lo]... [csum]
```

### `st::ecu::uds::UdsClient`

ISO 14229 services for VB and other CAN-only ECUs.

```cpp
class UdsClient {
public:
    explicit UdsClient(ITransport &t);

    Result<std::vector<std::uint8_t>> read_data_by_identifier(std::uint16_t did,
                                                                std::chrono::milliseconds timeout);
    Result<void>                       write_data_by_identifier(std::uint16_t did,
                                                                std::span<std::uint8_t const> data,
                                                                std::chrono::milliseconds timeout);

    Result<std::vector<std::uint8_t>> request_download(/* memory address, size */);
    Result<void>                       transfer_data(/* block */);
    Result<void>                       request_transfer_exit();

    Result<void> security_access(std::uint8_t level, SeedKeyHandler const &handler);
};
```

## Threading model

Per `docs/02-architecture.md`:

- **One I/O thread per opened device.** Owns the OS handle and the only thread allowed to call into the adapter's vendor SDK. This avoids reentrancy bugs on USB and serial APIs that aren't thread-safe.
- **A typed command queue** (`concurrent_queue<Command>`) sits between the calling thread(s) and the I/O thread. `send_recv` posts a command and waits on a future returned by the queue.
- **Streaming mode** uses a single-producer-single-consumer **lock-free ring buffer** (~64K frames) between the I/O thread and the consumer (UI gauge update or CSV sink). The producer never blocks; the consumer drops frames if it falls behind, with a per-second drop counter surfaced to the user.

Cancellation:

- Every public method takes a timeout. On timeout the in-flight command is abandoned but the I/O thread continues working (closing and reopening the device on every timeout would be punishing on flaky USB).
- A separate `cancel()` method on long-running operations (flash) calls into the vendor SDK's cancel where supported, and otherwise marks the operation for abandonment as soon as the next bus event arrives.

## Error model

Per `st::core::Error`, with reserved codes from the 400s already laid out in `error.hpp`:

| ErrorCode | Meaning | Layer |
|---|---|---|
| `TransportUnavailable` | Adapter not connected, USB/serial gone, vendor SDK refused to open | Transport |
| `TransportTimeout` | `send_recv` returned no matching reply in `timeout` ms | Transport |
| `TransportNack` | Adapter returned a transport-level NACK (bus busy, no response) | Transport |
| `EcuRejected` | ECU returned a negative response code (NRC) at the application layer | ECU client |
| `SeedKeyFailed` | Authentication challenge failed | ECU client |

The `Error.message()` carries the raw NRC byte for `EcuRejected` so the user (or a higher layer) can disambiguate "service not supported" from "conditions not correct."

## Mock transport (testing without hardware)

```cpp
class MockTransport : public ITransport {
public:
    // Programmatically queue responses. Each enqueued frame matches the next
    // outgoing send. Mismatches are reported as a test failure via REQUIRE.
    void expect_send_recv(std::span<std::uint8_t const> request,
                          std::span<std::uint8_t const> response);

    // Replay mode: load a captured trace from disk and play it back as the
    // device-under-test sends matching requests.
    Status replay(std::filesystem::path const &trace);
};
```

This is what lets every SSM/UDS client test run in CI on machines that will never see a real ECU. Captured traces from real cars (when we have them) live in `fixtures/private/` and don't ship.

## Datalogging architecture

Concretely:

- The user picks a set of PIDs from the definition pack's `[[pid]]` entries.
- `LogSession::start(pids)` builds an SSM/UDS request batch covering those PIDs.
- The session's I/O thread loops: `send_recv → produce samples into ring buffer → repeat`. The cycle time depends on adapter throughput; we measure and report the achieved rate.
- The UI (or `subuwutuner-cli log`) reads from the ring buffer at its own pace. UI samples a snapshot for gauge update; CSV/FlatBuffers sink drains continuously.
- A separate I/O-thread-side `timestamp` is taken at frame arrival; we don't trust the UI thread's wall clock for sample timing.

## Suggested order of implementation (when the OBDX adapter arrives)

1. `st::transport::ITransport` interface + `MockTransport` only. Tests for the contract.
2. `st::ecu::ssm::SsmClient` against `MockTransport` with canned VA traces.
3. `st::transport::obdx` adapter — the developer's own adapter, the easiest to iterate against.
4. Real-car smoke test: `subuwutuner-cli read-rom <output.bin>` reads the developer's car using OBDX.
5. `st::transport::j2534` adapter (Tactrix OP2.0) — broader community coverage.
6. `st::log::LogSession` with the ring-buffer pipeline.
7. `subuwutuner-cli log --pid rpm,iat,maf --rate 100 --csv out.csv` for headless datalogging.
8. `st::ecu::uds::UdsClient` for VB.
9. Phase 4 (flashing) gates on all of the above plus the safety story in `docs/08-testing-strategy.md` Tier 4.

## Open questions

- **Tactrix runtime on macOS arm64.** Their Mac driver historically lagged. May require shipping x64-only on Mac for v1, or pushing OBDX as the recommended adapter for M-series users.
- **OBDX firmware versions.** Different firmware revisions may have subtly different STN extensions. Test against the dev's adapter and capture quirks as we find them.
- **Bench-mode (ECU on a bench harness) protocol.** The Phase-4 bench rig assumed in `docs/08` may need a different connect handshake than an in-car ECU; design TBD until we have a bench ECU to experiment with.
