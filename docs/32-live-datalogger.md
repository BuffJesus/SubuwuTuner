# 32 — Live datalogger GUI

> The live-streaming counterpart to the existing CSV-replay datalog
> panels (Knock, Adaptive History, Cold-Start, EBCS). Design landed;
> implementation queued. Closes the v1.0 "live gauge cluster + CSV
> log export" deliverable from `docs/04` Phase 3.

## What ships today vs what doesn't

**Already shipped (verified):**

- `st::log::LogSession` — I/O loop that owns a transport, walks a
  list of `LogChannel`s, emits samples to a `LogStream`.
- `st::log::CsvSink` — writes a `LogStream` to CSV, one row per
  sample tick. Used by the autotune kernels' replay path.
- `obdx::Transport::start_streaming` / `stop_streaming` — spawns a
  reader thread parsing every RxSmall push, populates `Frame{data,
  can_id, arrived}`, invokes the callback. Listen-only mode
  available for sniff workflows.
- `MockTransport::start_streaming` — synchronous, deterministic;
  the entire test surface uses it.

**Missing:**

- `j2534::Transport::start_streaming` — `NotImplemented` stub.
  Blocked on platform DLL dynload (`docs/04` Phase 3 line 51) which
  is itself adapter-blocked.
- `native::Transport::start_streaming` — same story for the
  doc-18 handheld codec; lower priority than j2534.
- Live gauge cluster GUI — no panel exists. The CSV-replay datalog
  panels (Knock / Adaptive / Cold-Start / EBCS) all read a file, not
  a live `LogSession`. The shape they assume (file → analysis →
  static display) doesn't fit a continuously-updating live view.
- A lock-free ring buffer between LogSession's I/O thread and the
  UI's render thread. The §5 performance target ("Lock-free sample
  ring buffer; UI samples a snapshot, never blocks the I/O thread")
  needs concrete code.

This doc covers the missing pieces.

## Architectural fit

```
                                ┌─────────────────┐
                                │  ITransport     │ (OBDX, J2534, native)
                                │  start_streaming│
                                └────────┬────────┘
                                         │ Frame callbacks (I/O thread)
                                         ▼
                                ┌─────────────────┐
                                │  LogSession     │
                                │  io_loop()      │ (background thread)
                                └────────┬────────┘
                                         │ Sample{ts, channel, value}
                                         ▼
                ┌────────────────────────┴────────────────────────┐
                │                                                  │
                ▼                                                  ▼
        ┌──────────────┐                              ┌────────────────────┐
        │  CsvSink     │ → file                        │  LiveBuffer        │
        │  (existing)  │                              │  (NEW)             │
        └──────────────┘                              │  ring-per-channel  │
                                                       └─────────┬──────────┘
                                                                 │ snapshot()
                                                                 ▼ (UI thread, 60 fps)
                                                       ┌────────────────────┐
                                                       │  Gauge cluster     │
                                                       │  panel             │
                                                       └────────────────────┘
```

Key decision: **`LiveBuffer` is its own sink**, parallel to
`CsvSink`. Both are `ILogSink` implementations; a single
`LogSession` can fan-out to one or both simultaneously. Recording to
CSV while a gauge cluster shows live values is the obvious "I'm
driving + logging" workflow.

## LiveBuffer — the lock-free ring

One ring buffer per channel. Each ring holds a fixed-N (default
8192) `Sample{timestamp, value}` pairs in a single-producer /
single-reader SPSC pattern: the I/O thread writes the tail; the UI
thread reads from any-index-up-to-tail. Wrap is handled by an
atomic write index.

```cpp
namespace st::log {

class LiveBuffer : public ILogSink {
public:
    struct Sample {
        std::chrono::steady_clock::time_point ts;
        double value;
    };

    explicit LiveBuffer(std::vector<LogChannel> channels,
                        std::size_t per_channel_capacity = 8192);

    // ILogSink — called by LogSession's I/O thread.
    void accept(SampleBatch const &batch) override;

    // UI-thread API. snapshot() returns a copy of the most recent
    // `count` samples for `channel_idx` in chronological order
    // (oldest first). Lock-free; the I/O thread continues writing
    // during the snapshot — late arrivals appear in the next read.
    [[nodiscard]] std::vector<Sample> snapshot(std::size_t channel_idx,
                                               std::size_t count) const;

    // For the gauge headline value — most recent sample for the
    // channel, or nullopt if none yet.
    [[nodiscard]] std::optional<Sample> latest(std::size_t channel_idx) const;

    [[nodiscard]] std::size_t channel_count() const noexcept;

private:
    struct Channel {
        std::atomic<std::uint64_t> write_index{0};
        std::vector<Sample> ring; // size = per_channel_capacity (pow2 if possible)
    };
    std::vector<Channel> channels_;
    std::size_t capacity_;
};

} // namespace st::log
```

The SPSC discipline is enforced by construction:
- One producer: `LogSession::io_loop()` is the only `accept()`
  caller; LogSession owns the sink exclusively.
- One reader: `snapshot()` / `latest()` are UI-thread-only. The
  panel that owns the gauge cluster is the only caller.

Memory ordering: writes use `release` on write_index; reads use
`acquire`. The samples themselves are POD with no aliasing
concerns. No mutex, no `std::shared_mutex`, no `std::lock_guard`.

## Gauge cluster panel — shape

A docked ImGui panel with N gauges in a grid. Each gauge:

```
┌─────────────────────────────────┐
│  RPM                            │  ← channel name (LogChannel.name)
│                                 │
│      4 250                      │  ← latest value (large font)
│      rpm                        │  ← unit (LogChannel.units)
│                                 │
│  ┌─────────────────────────┐    │
│  │ ▁▂▃▅▆▆▇▇▆▅▄▃▂▁         │    │  ← ImPlot mini-line, last 60 s
│  └─────────────────────────┘    │
│                                 │
│  range: 800–7100  red: >6800    │  ← bounds + redline (from pack)
└─────────────────────────────────┘
```

Layout: ImGui `Columns` (or `Tables`) of 4 wide on a 1080p display,
3 wide if the dock is narrower, 2 wide minimum. Each gauge is a
fixed 320x200 cell. Gauge headers (channel name + unit + redline)
come from the `LogChannel` definition + the pack's per-channel
metadata (today the pack carries name + units; redline + warn
thresholds get added as `[[log_channel]]` optional fields).

Rendering rate: 60 fps. The panel calls `LiveBuffer::latest()` for
each gauge's headline value + `LiveBuffer::snapshot(channel_idx,
last_60s_worth)` for the mini-line. Both are O(1) and O(N) on the
copy respectively — at 100 Hz × 60 s × 8 channels = 48 000 samples
per render, well inside an integrated-GPU's frame budget.

## Live-vs-replay parity

The four CSV-replay panels (Knock / Adaptive / Cold-Start / EBCS)
each take a `[[file path → analysis → static charts]]` shape. The
live gauge cluster takes a `[[LiveBuffer → headline + mini-line]]`
shape. They are not the same view — replay is offline analysis;
live is real-time monitoring. Both are useful; both ship.

A future "live-classified knock" panel could combine the two:
classify samples in the I/O thread (or a dedicated worker thread
fed from a second LiveBuffer-style ring) and overlay flagged
events on the gauge mini-line. Out of scope for v1.0.

## Recording to CSV while gauging

The user picks "Record this session" in the gauge panel's toolbar.
The LogSession is configured with TWO sinks: a CsvSink writing to
the user's chosen path AND the LiveBuffer feeding the gauges. Both
receive every sample. The gauge panel doesn't know or care if
recording is on; the CsvSink doesn't know or care if anyone is
watching.

Implementation note: `LogSession` currently takes a single
`LogStream`. Refactor to take a `std::vector<std::unique_ptr<ILogSink>>`
(or equivalent). `LogStream` becomes one ILogSink implementation;
CsvSink + LiveBuffer become others. Existing callers passing a
single LogStream stay working via a one-element vector.

## Channel definition extensions

The pack's `[[log_channel]]` entries pick up two optional fields:

```toml
[[log_channel]]
name        = "RPM"
units       = "rpm"
mode        = 0x22         # existing — UDS RDBI or SSM A8 selector
did         = 0xF20C       # existing
redline     = 6800.0       # NEW — gauge mini-line shades > this red
warn_above  = 5500.0       # NEW — shades amber between warn_above and redline
warn_below  = 800.0        # NEW — shades amber below warn_below (for AFR, etc.)
```

Existing packs without the new fields work fine — the gauge just
omits the threshold-shading band.

## Sustained 100 Hz target

The §5 performance target ("Sustained 100 Hz on supported
adapters") is achievable today on OBDX VX with a careful channel
selection (≤ 20 PIDs). The bottleneck is the ECU's response rate,
not the host pipeline. The pipeline itself:

- OBDX VX RX → frame callback: ~150 µs per frame.
- Frame callback → LogSession parse → LiveBuffer accept: ~10 µs.
- LiveBuffer snapshot (8 channels × 6000 samples each): ~200 µs.
- ImPlot render of 8 gauges: ~3 ms at 1080p on integrated GPU.

Total: ~3.4 ms per frame, well under the 16.6 ms 60-fps budget,
with 13 ms of headroom for everything else the UI does. The §5
"never blocks the I/O thread" property follows from the SPSC ring
discipline — the I/O thread's `accept()` is wait-free.

## Sequencing

1. **`LiveBuffer` + tests** — pure SPSC ring, no UI. Tests cover
   wrap-around, snapshot consistency under concurrent writes
   (via std::thread), latest()-after-empty, capacity rounding to
   power-of-two for fast modulo. ~1 day.
2. **`LogSession` multi-sink refactor + tests** — accept
   `std::vector<unique_ptr<ILogSink>>`, fan-out in the I/O loop.
   Existing single-LogStream callers wrap as one-element vectors.
   Tests confirm fan-out emits to both sinks for the same sample.
   ~½ day.
3. **Gauge cluster panel + AppState wiring** — new file
   `src/ui/src/panels/gauge_cluster.cpp`. Channel selection modal
   reuses the existing `adapter_picker.hpp` pattern. Toolbar:
   start/stop session, channel picker, record-to-CSV toggle, clear
   buffer. ~2 days.
4. **`[[log_channel]]` redline/warn fields + pack-loader update**
   — small `Definition` schema bump. Tests verify packs without
   the fields load cleanly (back-compat). ~½ day.
5. **`subuwutuner-cli log` toolbar parity** — the CLI's existing
   `log` subcommand stays as-is; the GUI just adds the live-view
   counterpart. No CLI changes needed for the gauge cluster
   itself.
6. **j2534::Transport streaming** — implement the read-thread
   loop calling `PassThruReadMsgs` in a loop, emitting frames via
   the callback. Adapter-blocked on hardware validation.
   Hardware-free unit tests via a mock `J2534Library` shipping
   pre-canned frames into the callback's queue. ~1 day.

Total: ~5 hardware-free days for items 1–5; item 6 is ~1 day +
bench-rig validation.

## Test plan

Hardware-free:

- **SPSC ring under concurrent writes**: spawn a writer thread
  appending samples for 1 s; UI thread snapshots every 16 ms;
  assert (a) snapshots are monotonically increasing in size up to
  capacity, (b) no torn samples observed (the SPSC discipline
  guarantees this by construction; the test is a sanity check).
- **Wrap-around**: write 2× capacity samples; snapshot(channel,
  capacity) returns exactly the most recent capacity samples in
  order.
- **LogSession multi-sink fan-out**: register a LiveBuffer + a
  recording CsvSink; emit N samples; both sinks observe N
  identical samples.
- **Gauge panel render smoke**: feed a LiveBuffer with a
  precomputed sample stream; render N frames; no ImGui asserts.
- **Channel field back-compat**: load a pack without redline /
  warn_above / warn_below; gauge renders without crashing and
  without shading bands.

Hardware-gated (OBDX VX in hand):

- **End-to-end live gauge** against a running ECU. Pick 8 PIDs
  from the LF79103P pack (RPM, MAP, TPS, IAT, ECT, MAF, AFR,
  ignition advance). Start session, hold idle, blip throttle.
  Visual confirmation: RPM gauge tracks, others move with engine
  state. CSV recorded in parallel matches the gauge values
  within timestamp tolerance.
- **Sustained-100 Hz check** against the same 8 PIDs. Run for 60 s.
  Measured rate ≥ 90 Hz mean, no dropped frames in the UI, ring
  never overflows (8192-sample capacity covers ≥ 80 s at 100 Hz).

## Open questions

- **Auto-pause on focus loss.** When the user alt-tabs, should the
  session keep streaming + recording, or pause? Recommend: keep
  streaming (a recording session shouldn't depend on window focus),
  but throttle UI render to 5 fps via ImGui's `WantSaveIniSettings`
  pattern.
- **Gauge layout persistence.** Per-pack? Per-project? Per-install?
  Recommend per-project — the gauge selection is part of "I'm
  tuning this car for this purpose right now."
- **High-rate channels.** Some signals (knock-windowed cyl pressure)
  want > 100 Hz. The pipeline supports it; the gauge cluster's
  60-sample-mini-line is overkill at 1 kHz. Recommend: per-channel
  display-rate decimation in the gauge panel (capture rate stays
  full; visual rate caps at 100 Hz for the mini-line).

## References

- `docs/04-roadmap.md` Phase 3 — live datalog gauge cluster + CSV
  log export goals.
- `docs/05-improvements.md` §5 — real-time datalogging quality
  targets (sustained 100 Hz, lock-free ring, UI never blocks).
- `docs/13-transport.md` — `ITransport::start_streaming` contract
  (callback runs on the transport's I/O thread; `stop_streaming`
  must wait for the callback to drain).
- `docs/19-live-tuning.md` — the *write* counterpart of this
  *read* path. Both lean on the same LogSession plumbing.
- `src/log/include/st/log.hpp` — existing LogSession + CsvSink
  contracts.
- `src/transport/src/obdx_transport.cpp` — reference
  start_streaming implementation (the j2534 + native variants
  follow the same shape).
- `fixtures/private/findings_signals/` — 165 A-series + 786
  B-series RAM signal catalogs (read-on-demand surface). The
  channels users will most commonly pick for the gauge cluster.
