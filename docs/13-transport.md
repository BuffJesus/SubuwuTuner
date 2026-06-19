# 13 — Transport & ECU Protocol Design

This document captures the design for Phase 3 (comms and datalogging). The Phase 2 calibration side (ROM I/O, definitions, editing, projects) is complete; everything below describes how SubuwuTuner talks to a real ECU through a real adapter.

Status today (2026-05-26) — the **shapes** are real and locked under unit tests. OBDX Pro VX is in hand (2026-05-24), so the **platform wiring** to actual hardware (libusb CDC channel + J2534 DLL dynamic-load shim) is the active task; Tactrix OP2.0 remains the second-source path:

| Piece | Status |
|---|---|
| `st::transport::ITransport` interface + contract tests | ✅ shipped |
| `st::transport::MockTransport` (queue + replay) | ✅ shipped |
| `st::transport::IByteChannel` (USB CDC byte abstraction) | ✅ shipped |
| `st::transport::open_transport` factory + CLI `--transport <kind>` flag | ✅ shipped |
| `st::transport::j2534::Transport` (skeleton + DLL discovery) | 🟡 shipped, gated on `LoadLibraryA` of a real vendor DLL |
| `st::transport::obdx::Transport` + DVI codec | ✅ shipped end-to-end on Windows — operational against the user's 2017 WRX (ROM dumps, install-flow sniff captures, live SA L3 read). POSIX byte-channel impl is the only remaining gap. |
| `st::transport::ap3` USB byte-channel + wire codec (COBB AccessPort V3 file vault) | ✅ shipped — libusb-backed `IByteChannel` (VID 0x1A84 / PID 0x0121, bulk OUT 0x03 / IN 0x82), wire codec (sync + u24-BE wire_len + CRC-32) and `st::devices::ap3::Client` for the Capability A surface (`ls`/`pull`/`push`/`rm`/`state`/`backup`). Default-on; `.ptm` cipher introspection gated behind `ST_ENABLE_COBB_AP_CIPHER`. See `docs/34-cobb-ap-as-tune-vault.md`. |
| `st::transport::native::Transport` + SOF/seq/CRC16 codec | 🟡 shipped, gated on doc-18 handheld firmware (not on the byte-channel layer, which works) |
| Win32 serial `IByteChannel` (`CreateFile` + DCB + COMMTIMEOUTS) | ✅ shipped (`src/transport/src/serial_byte_channel_win.cpp`) |
| POSIX serial `IByteChannel` (termios) | ⬜ stub returns `NotImplemented`; lands when first Linux/macOS user needs it |
| `st::ecu::ssm::SsmClient` (K-Line + CAN paths) | ✅ CAN path validated against real ECU (SSM/UDS sniff captures); K-Line still MockTransport-only |
| `st::ecu::uds::UdsClient` | ✅ validated against the user's 2017 WRX over OBDX (SecurityAccess, RDBI, RMBA, ReadFullRom via UDS RequestDownload/TransferData) |
| `st::ecu::uds` OBD-II Mode 0x09 (CAL ID / CVN / VIN) | ✅ shipped + CLI surfaced |
| `st::ecu::subaru_security` (SecurityAccess Feistel: factory + aftermarket L1/L3 variants, CLI `--sa-variant`) | ✅ shipped |
| `st::ecu::bulk_reflash` (gated 0xB6 cipher, `ST_ENABLE_BULK_REFLASH_CIPHER`) | ✅ shipped, off in default builds |
| `st::log::LogStream` (SPSC ring) + `LogSession` (I/O thread) + `CsvSink` | ✅ shipped; consumed by MockTransport-backed tests |
| `subuwutuner-cli log --def <pack> --pid <ids> [--csv …]` | ✅ shipped; gates on transport platform wiring for live ECU |
| ELM327 (read-only) | ⬜ deferred — not blocking VA/VB targets |
| OBDLink (STN) | ⬜ deferred |

Writing this design serves two ongoing purposes: (1) it defines the shape concrete adapters port into, and (2) it ensures the calibration code we've already shipped stays compatible with the transport layer's needs.

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

## Module map (as-shipped)

Everything below lives under `src/transport/` (single library, internal subdirectories) — we collapsed the per-adapter top-level libs from the original design once it was clear they all share `IByteChannel`, the factory, and `Frame`. The flat layout keeps the CMake target count down and lets the framing codecs (`obdx_dvi.cpp`, `native.cpp`) be unit-tested without dragging the full Transport in.

```
src/
├── transport/                          (st::transport — library)
│   ├── include/st/
│   │   ├── transport.hpp               ITransport, Frame, LinkConfig
│   │   └── transport/
│   │       ├── byte_channel.hpp        IByteChannel (USB CDC / serial seam)
│   │       ├── factory.hpp             Kind / TransportSpec / open_transport
│   │       ├── mock.hpp                MockTransport (queue + replay)
│   │       ├── j2534.hpp               J2534Library (vendor DLL wrapper)
│   │       ├── j2534_discovery.hpp     Windows registry walk for PassThruSupport.04.04
│   │       ├── j2534_transport.hpp     ITransport on top of J2534Library
│   │       ├── obdx_dvi.hpp            DVI codec (request/response framing)
│   │       ├── obdx_transport.hpp      ITransport on top of IByteChannel + DVI
│   │       ├── native.hpp              Native codec (SOF/seq/opcode/LEN/CRC16)
│   │       └── native_transport.hpp    ITransport on top of IByteChannel + native codec
│   └── src/                            matching .cpp per header
├── ecu/                                (st::ecu)
│   ├── include/st/ecu/
│   │   ├── ssm.hpp                     SsmClient — K-Line + CAN
│   │   └── uds.hpp                     UdsClient — ISO 14229
│   └── src/                            ssm.cpp, uds.cpp
└── log/                                (st::log)
    ├── include/st/
    │   └── log.hpp                     LogStream (SPSC ring), LogSession (I/O thread), LogChannel, CsvSink
    └── src/                            log.cpp
```

The `st::transport` library depends only on `st::core`. Adapter implementations live as siblings inside `st::transport::{j2534,obdx,native}` rather than separate top-level libs. `st::ecu::ssm` and `st::ecu::uds` depend on `st::transport` only, never on a specific adapter — this is what lets the same `SsmClient` talk through any compatible adapter.

### Why `IByteChannel` is a separate seam

The framing codecs (`obdx_dvi`, `native`) need to test cleanly without a real USB device. `IByteChannel` lets the codec wrap *any* byte sink — a fake in-memory `vector<uint8_t>` for unit tests, libusb for production, a TCP socket for a future networked-adapter shim. J2534 stays outside this abstraction because the vendor DLL owns the byte plumbing internally; we call function pointers, not byte reads.

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
    int      baud{};        // SSM K-Line: 4800 (Subaru's spec; RR's SSMProtocol.java
                            //   hardcodes 4800/8N1, 2000 ms connect, 55 ms send).
                            //   KWP2000/ISO-14230 K-Line on other platforms: 10400.
                            //   CAN: 500000 typical.
    int      can_id_request{};  // for CAN only — Subaru SSM-on-CAN uses 0x7E0 / 0x7E8
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
    // the FIRST reply frame matching the implicit response context (e.g. a
    // specific SSM responder address or UDS positive response).
    //
    // ISO 14229 §7.5 NRC 0x78 (responsePending) contract — round-28 + 53:
    // once the first frame arrives, if it's NRC 0x78 the implementation
    // swallows intermediate 0x78s and waits for the final response. Each
    // 0x78 effectively resets the P2*_Server_max deadline, capped at 30 s
    // total. So the effective worst-case wait is `timeout + 30 s` for a
    // chatty handler (e.g. 0x31 erase, large 0xB6 write).
    //
    // Picking `timeout`:
    //   short queries (RDBI, DSC, 0x37 close):  3 s is fine
    //   long ops (0xB6 256-byte, 0x31 erase):   10 s+ — otherwise the 3 s
    //                                            verb-side wait pre-empts
    //                                            the swallow loop on a
    //                                            silent ECU (round-52 §4)
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

### OBDX Pro VX (STN2120 silicon; native USB / Wi-Fi / BT)

OBDX runs the STN2120 chip but the firmware presents its **own** command set — *not* the STN command set documented by Scantool.net (no `STSBR`/`STCFCP`/`STMA`). Two modes:

- **ELM** — string-based, ELM327 v1.4-compatible AT command set, plus a small `DX *` extension set (`DX DP`, `DX SD`, `DX VS1/4`, `DX PT0/1`, `DX SM`, `DX RA`, `DX RE`, `DX US`, `DX I`). Default mode at power-on; prompts with `>`.
- **DVI** (Direct Vehicle Interface) — binary, framed, much faster. Enabled by sending the ELM string `DX DP 1`. **This is the mode we target.**

DVI framing (per *OBDX Pro VT Command Set Reference Guide v1.06*, the public spec):

```
Request:  CMD  LEN  [DATA…]  CHK
Response: (CMD|0x10)  LEN  [DATA…]  CHK     ; response opcode = request opcode with bit 4 set
Error:    0x7F  LEN  CMD  ERRCODE  CHK
Checksum: sum of all preceding bytes, then bitwise NOT, kept as a uint8.
```

Documented opcodes we'll use:

| Opcode | Name | Notes |
|---|---|---|
| `0x08` | RX from network (small, ≤255 B length) | normal datalogging path |
| `0x09` | RX from network (large, 2-byte length) | for fragmented or batched reads |
| `0x10` | TX to network (≤255 B) | normal request emission |
| `0x11` | TX to network (large, up to 12000 B) | bulk uploads — relevant for Phase 5 flash blocks |
| `0x22` | Scantool info (sub-ops 0x01–0x06: HW / FW / model / name / serial / supported protocols) | capability probe at session start |
| `0x24` | Settings (sub-op `0x03` = enable µs-resolution RX timestamp) | turn on at log-session start so frame arrival times come from the adapter, not the host |
| `0x25` | Soft reboot back to ELM mode | clean shutdown |
| `0x31` | Set OBD protocol + enable/disable network + switch ELM↔DVI | post-handshake configuration |
| `0x33` | Filter + protocol settings (titled "VPW Specific Settings" in VT v1.06 §3.11, but sub-ops 0x00–0x04 — To/From filter, To/From range filter, Mask — are generic filter primitives). | sub-ops 0x06–0x0F are VPW-scoped (4x speed, CRC, 1x/4x timings, error bits); the filter primitives' CAN byte semantics on the VX are not in the VT PDF |
| `0x3A` | ADC — read DLC pin 16 voltage | battery-health check before flash |

Implementation strategy:

- Open the USB CDC ACM endpoint via libusb (Windows + Linux) or the platform native serial driver (macOS). BT and Wi-Fi route to virtual COM ports we treat identically.
- Handshake in ELM mode: send `AT @1` (probe), confirm OBDX signature, send `DX DP 1` to switch into DVI, then drive everything else via the binary opcodes above.
- ISO-TP fragmentation is handled by the adapter when configured; we receive whole application-layer responses. CAN filter / flow-control IDs go in via `0x31` setup + filter table.
- VX supported-protocol matrix is **HSCAN + J1850 VPW + GM UART ALDL only** (no K-line / SSM-over-K). VA/VB WRX is HSCAN-only so the VX covers our v1.0 targets; legacy Subaru SSM2-over-K-line is out of scope for this adapter and we steer those users to OpenPort 2.0.

Risks:

- The GitHub org (`github.com/OBDXPro`) was effectively dormant 2023-05 → 2025-11 on source-bearing repos; treat the v1.06 PDF as the stable spec and budget for trial-and-error on undocumented edges (CAN-side ISO-TP setup details, firmware-version branching, exact USB framing on the wire).
- Their J2534 driver is also closed-source but exposes a debug-log toggle at `%AppData%\OBDX Pro\J2534\Settings\OBDXFT_Config.cfg` (`LoggingEnabled=1` → logs land in `…\J2534\Logs`). Useful for diagnosing other tools' behavior against an OBDX, even though we won't go through their J2534 layer ourselves.

Clean-room boundary (per `docs/15-clean-room-engineering.md`):

- The **VT v1.06 PDF** is a public protocol spec with no embargo clause — implement against it freely, cite it in source comments.
- The `OBDXPro/OBDX-Templates` C# samples have **no SPDX license file** and only a README sentence permitting use in commercial products. Treat them as API-shape reference only: one engineer reads them to understand the lifecycle (search → connect → set-protocol → set-filter → enable → write/read), writes a plain-English spec, and a different engineer implements the bytes from the PDF.
- The `OBDXPro/J2534` installers and `OBDXPro/OBDX-Pro-VX-GT-FT-USB-Driver` repo are **all-rights-reserved binaries** ("Redistribution, modification, reverse engineering, or derivative works are strictly prohibited"). Use as-shipped; do not decompile. We don't depend on them in our pipeline.
- The closed-source `OBDXWindows` / `OBDXMAUI` NuGets are .NET-only, last updated 2023-06, and unsuitable for our C++/cross-platform target. Skip.

### OBDX J2534 vs. native DVI

OBDX *also* ships a J2534 v04.04 DLL, and we have a `transport_j2534` path. The catch: per the VT v1.06 PDF §1.2, **the OBDX J2534 DLL is internally just a DVI multiplexer** — it speaks the same DVI bytes we'd speak directly. Going through their J2534 layer adds a Windows-only dependency on their installer for zero protocol gain. So `transport_obdx` targets DVI direct over USB CDC, and a user with an OBDX adapter routes through `transport_obdx`, not `transport_j2534`. Tactrix users go through `transport_j2534`.

### OBDLink (STN-series — STN1110, STN2100, etc.)

OBDLink shares the STN chip family with OBDX but ships the **real** STN command set (`STSBR`, `STCFCP`, `STMA`, `STPX`, etc. per Scantool.net's docs), not OBDX's `DX *` extensions. So `transport_stn` and `transport_obdx` share USB CDC handling but diverge on the command vocabulary above — no shared protocol code.

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

    // Block read (READ_MEMORY, 0xA0) when the link supports it; otherwise loops
    // single-address reads (READ_ADDRESS, 0xA8). K-Line tops out at 128 bytes
    // per block read (the request's num_bytes field is one byte holding N-1);
    // larger lengths must be chunked. SSM-on-CAN does not implement block
    // operations — the implementation must fall back to per-byte 0xA8 there.
    Result<std::vector<std::uint8_t>> read(std::uint32_t address, std::size_t length,
                                            std::chrono::milliseconds timeout);

    // Block write (WRITE_MEMORY, 0xB0) when supported; otherwise per-byte
    // single-address write (WRITE_ADDRESS, 0xB8). Same CAN restriction as read:
    // block write is K-Line-only. Returns the bytes the ECU echoed, which is
    // how we confirm the write took.
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

K-Line opcodes (host → ECU / ECU → host):

| Direction | Name                    | Opcode |
|-----------|-------------------------|--------|
| Request   | `READ_MEMORY` (block)   | `0xA0` |
| Request   | `READ_ADDRESS` (single) | `0xA8` |
| Request   | `WRITE_MEMORY` (block)  | `0xB0` |
| Request   | `WRITE_ADDRESS` (single)| `0xB8` |
| Request   | `ECU_INIT`              | `0xBF` |
| Response  | `READ_MEMORY_RESPONSE`  | `0xE0` |
| Response  | `READ_ADDRESS_RESPONSE` | `0xE8` |
| Response  | `WRITE_MEMORY_RESPONSE` | `0xF0` |
| Response  | `WRITE_ADDRESS_RESPONSE`| `0xF8` |
| Response  | `ECU_INIT_RESPONSE`     | `0xFF` |

SSM-on-CAN uses **different opcodes** — do not assume the K-Line value works
on CAN. CAN init is `0xAA` request / `0xEA` response; only single-address read
(`0xA8`) and single-address write (`0xB8`) are implemented in any public
SSM-on-CAN code. The CAN transport handles SSM's header/tester/length/checksum
implicitly via ISO-TP, so the CAN payload is just `<opcode> <args…>`.

Checksum (K-Line only — CAN doesn't need it): sum of every byte preceding the
checksum slot, kept modulo 2¹⁶, then truncated to one byte.

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
- The UI (or `subuwutuner-cli log`) reads from the ring buffer at its own pace. UI samples a snapshot for gauge update; the `CsvSink` (only sink shipped today) drains continuously to disk.
- A separate I/O-thread-side `timestamp` is taken at frame arrival; we don't trust the UI thread's wall clock for sample timing.

## Implementation order — done / pending

Done (shipped against MockTransport + tests):

1. ✅ `st::transport::ITransport` interface + contract tests.
2. ✅ `st::transport::MockTransport` (programmatic + replay).
3. ✅ `st::transport::IByteChannel` for the framed-codec adapters.
4. ✅ `st::ecu::ssm::SsmClient` (K-Line + CAN paths) — tests run through MockTransport with canned traces.
5. ✅ `st::ecu::uds::UdsClient` for VB — tests run through MockTransport.
6. ✅ `st::transport::obdx::Transport` + DVI codec skeleton.
7. ✅ `st::transport::native::Transport` + SOF/seq/CRC16 codec skeleton.
8. ✅ `st::transport::j2534::Transport` + Windows registry discovery skeleton.
9. ✅ `st::transport::open_transport` factory + CLI `--transport <kind>` plumbing.
10. ✅ `st::log::LogStream` + `LogSession` + `CsvSink` — SPSC ring, I/O-thread worker, drop counter, CSV streaming (validated under MockTransport).
11. ✅ `subuwutuner-cli log --def <pack> --pid <ids> [--csv …]` — headless datalogging entry point.

Pending (gated on hardware on the bench):

12. 🟡 Real-car smoke test: `subuwutuner-cli rom-pull --transport obdx --device COM5 --sa-variant <variant> --output <output.bin>` against the developer's car. This is the moment platform wiring (libusb open / `CreateFileW` on `COM*`) inside `obdx::Transport::open` flips from `NotImplemented` to live.
13. 🟡 Same smoke test through Tactrix OP2.0 — flips J2534 from `NotImplemented` to live via `LoadLibraryA` on the registered DLL.
14. 🟡 `subuwutuner-cli log` against a real car — exercises the LogSession pipeline end-to-end through whichever transport's platform layer comes online first.
15. ⬜ Phase 4 (flashing) gates on all of the above plus the safety story in `docs/08-testing-strategy.md` Tier 4.

## Sniff mode (passive bus monitor)

Set `LinkConfig::listen_only = true` to open a CAN transport in
passive-monitor mode. Three things change vs the normal active flow:

1. **No CAN filter** is configured at the adapter level (OBDX
   "Entire Filter" command 0x34 is skipped). Every CAN frame on the bus
   is pushed to the host regardless of CAN ID.
2. **EnableNetwork** uses STATE=0x02 (LISTEN-ONLY) instead of 0x01
   (ON), so the adapter is electrically silent on the bus. The MAC
   never transmits even an ACK bit. This is what makes it safe to
   share the OBD-II port with another active tool via a Y-cable —
   two transmitters fighting over the same CAN-H/CAN-L would collide.
3. **`send` / `send_recv` are disabled** in this mode (return
   `TransportUnavailable`). The only way to consume frames is via
   `start_streaming(callback)`.

When a frame arrives the callback gets a `Frame` whose `can_id` field is
populated (vs always-zero in active mode — active mode strips the CAN ID
after validating it matches `can_id_response`). The `data` field is the
raw bus payload including the ISO-TP PCI byte — sniff mode is for
observing bus state byte-for-byte, not for application-layer
deserialization.

`subuwutuner-cli sniff` is the user-facing entry point. See
`docs/23-security-access.md` for the SA capture workflow that motivated
this mode, and `tools/extract_subaru_sa.py` for the parser that turns a
sniff log into a JSON list of seed/key pairs.

## Open questions

- **Tactrix runtime on macOS arm64.** Their Mac driver historically lagged. May require shipping x64-only on Mac for v1, or pushing OBDX as the recommended adapter for M-series users.
- **OBDX firmware versions.** Different firmware revisions may have subtly different DVI quirks (the v1.06 PDF is the documented spec, but per-revision behavior on, e.g., timestamp resolution or the exact protocol-switch handshake may drift). Test against the dev's adapter on receipt, log the `0x22` sub-op `0x02` firmware string into a per-adapter quirks table.
- **Exact CAN-side DVI configuration sequence.** The v1.06 PDF documents VPW concretely (filter, CRC, 1×/4× timings) but is thinner on CAN ISO-TP setup — the `0x31` "set protocol + enable" plus filter table layout will need trial-and-error against a known-good CAN target before we trust it on Subaru. Capture a session log via the OBDX J2534 debug toggle as a reference trace.
- **Bench-mode (ECU on a bench harness) protocol.** The Phase-4 bench rig assumed in `docs/08` may need a different connect handshake than an in-car ECU; design TBD until we have a bench ECU to experiment with.
