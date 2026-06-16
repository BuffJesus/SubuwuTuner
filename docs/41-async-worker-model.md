# Async worker model for `st::devices::ets::Client`

## Overview

The COBB AccessPort V3 file-vault client (`st::devices::ets::Client`) owns a
single dedicated **worker thread** plus a FIFO **operation queue**. Every
public verb that touches the wire — `query_state`, `ls`, `read_file`,
`write_file`, `remove_file` — routes its actual I/O through that worker. The
constructor spins the thread up; the destructor signals stop and joins.

This was the T-track shipped on 2026-06-13 (T1 worker + queue, T2 verb
migration, T3 test coverage). It implements decision **C2** from the analyst's
strategic-decisions handoff
([`HANDOFF-from-analyst-2026-06-13-ap-browser-decisions.md`][handoff] §C),
which picked a single dedicated worker over per-op threads (C1) or coroutines
(C3).

[handoff]: ../../findings/handoffs/HANDOFF-from-analyst-2026-06-13-ap-browser-decisions.md

### Why a worker queue exists

The AP3 transport is **physically single-stream**. The i.MX28 inside the
device exposes one bulk OUT endpoint and one bulk IN endpoint; every
protocol op is a request/response pair on those two endpoints. There is no
way to interleave two ops at the wire layer — `cmd 0x21 (ReadFile DATA)`
must complete before `cmd 0x22 (PutFile setup)` begins.

The queue model exposes that fact honestly:

- **Concurrent submitters** can call `read_file_async` and `write_file_async`
  back-to-back from any thread; the worker serialises them at the queue.
  Callers don't have to coordinate their own mutex against the channel.
- **GUI frame loops** don't block. A 50 KB tune pull takes ~1 s of wall time
  on a live AP; without the worker that 1 s would freeze the editor. The
  async verbs return immediately and the GUI polls the future per frame.
- **CLI callers** (and existing single-threaded tests) pay zero refactor
  cost. The sync verbs (`ls`, `read_file`, …) keep their old blocking
  signature — they just enqueue + `future::get()` internally.

The alternatives were rejected for concrete reasons (per §C2 of the handoff):
per-op threads (C1) fight over the channel and don't compose to "pull A while
push B"; coroutines (C3) are correct in spirit but libusb's async API is
awkward to bridge to `co_await` cleanly, and the v1.1 budget is better spent
on UX than on transport-async plumbing.

## The contract

### Sync verb shape

```cpp
[[nodiscard]] Result<std::vector<FileInfo>> ls(std::string_view subdir);
[[nodiscard]] Result<std::vector<std::uint8_t>> read_file(std::string_view path);
[[nodiscard]] Status write_file(std::string_view path,
                                std::span<std::uint8_t const> data,
                                std::uint64_t mtime_unix_secs = 0);
[[nodiscard]] Status remove_file(std::string_view path);
```

Each sync verb internally calls its `_async` counterpart and immediately
waits on the future:

```cpp
Result<std::vector<FileInfo>> Client::ls(std::string_view subdir) {
    return ls_async(subdir).get();
}
```

Existing callers (the CLI, every prior test) see no behavioural change. The
sync verb blocks the calling thread until the worker has finished the op.

### Async verb shape

```cpp
[[nodiscard]] std::future<Result<std::vector<FileInfo>>>
    ls_async(std::string_view subdir);
[[nodiscard]] std::future<Result<std::vector<std::uint8_t>>>
    read_file_async(std::string_view path);
[[nodiscard]] std::future<Status>
    write_file_async(std::string_view path,
                     std::vector<std::uint8_t> data,
                     std::uint64_t mtime_unix_secs = 0);
[[nodiscard]] std::future<Status>
    remove_file_async(std::string_view path);
```

The async verbs return a `std::future<Result<T>>` (or `std::future<Status>`
for void-result ops). The future is signalled when the worker thread
finishes the op. Polling pattern below under *Future-based progress
polling*.

Note the `write_file_async` signature change: the async form takes
`std::vector<std::uint8_t>` **by value** instead of `std::span`, because the
worker needs to own the bytes past the caller's stack frame. The sync form
keeps the `std::span` signature and copies internally.

### FIFO ordering

The queue is strict FIFO. Three submissions complete in submission order:

```cpp
auto f1 = client.read_file_async("/maps/A.ptm");  // runs first
auto f2 = client.read_file_async("/maps/B.ptm");  // runs second
auto f3 = client.read_file_async("/maps/C.ptm");  // runs third
```

The futures themselves can be `.get()`'d in any order — that's just blocking
on completion — but the **channel** sees A, then B, then C. This matches the
AP firmware's request/response pairing: a partially-served `cmd 0x21` cannot
be interleaved with another op without dazing the device (see
[`docs/34-cobb-ap-as-tune-vault.md`][docs34] §"Malformed-body daze").

[docs34]: 34-cobb-ap-as-tune-vault.md

### RAII

The worker thread is owned by the `Client`. The destructor signals stop
under the queue mutex, notifies the condition variable, and joins:

```cpp
Client::~Client() {
    {
        std::lock_guard<std::mutex> lock{queue_mu_};
        stop_worker_ = true;
    }
    queue_cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}
```

The worker drains the in-flight task (if any) and any queued tasks the
stop flag arrived after, then exits its loop. The recommended usage is to
hold the `Client` as a local on the calling frame or as a member of the
panel/CLI subcommand that owns the AP3 session — the destructor's join
is the synchronisation point. Don't `new` a `Client` and forget about it.

`Client` is **non-copyable, non-moveable** for the same reason — the worker
thread captures `this`, so moving the object would invalidate the worker's
`this` pointer mid-flight.

### Channel ownership

Once `Client`'s constructor returns, the worker thread is the **only**
thread that touches the `IByteChannel`. Submitter threads only put callables
on the queue; they never call `channel_->read_bytes` / `write_bytes`
themselves.

This is the invariant that makes the queue a correctness construct, not just
an ergonomic one. The channel implementations (`LibusbByteChannel`,
`LoopbackByteChannel`) are **not** required to be thread-safe internally —
the worker model guarantees the channel sees only one caller at a time.

## When to use sync vs async

| Use case | Recommended verb |
|---|---|
| CLI subcommand (`subuwutuner-cli ap3 pull …`) | sync |
| Startup probe / `query_state` from `main` | sync |
| Unit tests with a `LoopbackByteChannel` | sync |
| GUI frame loop (Library panel, Browser panel) | async |
| Bulk operations (`ap3 backup`, batch-pull) | async + per-op polling |
| Library "Push to AP" action | async |

**Why sync for CLI:** a CLI subcommand runs in a process-per-op model. The
process IS the unit of concurrency; the OS scheduler runs N concurrent CLI
invocations in parallel as N processes. Inside one process, the work is
inherently serial — there's nothing else for the main thread to do while
the op completes. Sync is simpler and the future-overhead is wasted.

**Why sync for startup probes:** `query_state` typically completes in
~200 ms (cmd 0x28 + 0x04 + 0x03 + three status probes). A sub-second
blocking call from `main` before the GUI window even appears is invisible
to the user.

**Why sync for tests:** the test fixtures use `LoopbackByteChannel`, which
hands queued reply packets out instantly. Tests want deterministic ordering
+ "did the op succeed?" assertions; the future machinery is noise.

**Why async for GUI:** the SubuwuTuner editor runs at 60 FPS. A blocking
1 s call from the frame loop is a 60-frame freeze — visible, ugly, and
will get flagged in the first round of user feedback. Async + per-frame
poll keeps the frame budget at 16 ms even when a 50 KB read is in flight
on the worker.

**Why async for bulk:** `ap3 backup` lists every subdir, then pulls every
file. Done synchronously that's 60+ round-trips back-to-back; done async
with a progress callback it's the same wall time but with live progress
feedback to the user.

## Future-based progress polling pattern

A GUI panel that tracks one in-flight op holds the future on its state
struct and polls it from the frame tick:

```cpp
struct LibraryPanelState {
    std::future<Result<std::vector<std::uint8_t>>> in_flight;
    std::string last_error;
    bool busy() const { return in_flight.valid(); }
};

void LibraryPanel::tick(LibraryPanelState& state, Client& client) {
    using namespace std::chrono_literals;

    // Poll: did the current op finish this frame?
    if (state.in_flight.valid() &&
        state.in_flight.wait_for(0ms) == std::future_status::ready) {
        auto result = state.in_flight.get();
        state.in_flight = {};  // mark idle
        if (result) {
            handle_pulled_bytes(*result);
        } else {
            state.last_error = result.error().message();
        }
    }

    // User clicked "Pull tune from AP" while no op was in flight?
    if (ui_pull_clicked() && !state.busy()) {
        state.in_flight = client.read_file_async(selected_path());
    }
}
```

Key points:

- `wait_for(0ms)` is non-blocking — it returns `timeout` if the worker
  hasn't finished yet, `ready` if it has. The frame loop never blocks.
- Calling `wait_for` on an invalid future is undefined behaviour, so guard
  with `valid()` first. Once `get()` is called, the future is invalidated
  (`valid()` returns false) — reassign from `{}` to make the state explicit.
- The "is there an op in flight?" question collapses to `state.in_flight
  .valid()`. No separate bool flag needed.
- The result type is whatever the async verb returned: `Result<T>` for
  `ls_async` / `read_file_async`, `Status` for `write_file_async` /
  `remove_file_async`.

For panels that pipeline multiple ops (e.g. backup-all iterating over a
list of files), keep a queue-of-futures and dispatch the next op when the
current one resolves:

```cpp
struct BackupAllState {
    std::deque<std::string> remaining_paths;
    std::future<Result<std::vector<std::uint8_t>>> in_flight;
};

void tick_backup(BackupAllState& s, Client& client) {
    using namespace std::chrono_literals;

    if (s.in_flight.valid() &&
        s.in_flight.wait_for(0ms) == std::future_status::ready) {
        auto result = s.in_flight.get();
        write_to_disk(result);   // application-level
        s.in_flight = {};
    }
    if (!s.in_flight.valid() && !s.remaining_paths.empty()) {
        auto next = std::move(s.remaining_paths.front());
        s.remaining_paths.pop_front();
        s.in_flight = client.read_file_async(next);
    }
}
```

This is the pattern the Library panel's "Sync with connected AP" action
will use (B2 from the handoff, in flight on the implementer side).

## Threading invariants

The matrix of what is and isn't safe to do:

| Situation | Safety | Why |
|---|---|---|
| Multiple `Client` instances sharing one `IByteChannel` | **Unsafe** | Each Client's worker thinks it owns the channel; they race |
| Multiple threads submitting to one `Client` | **Safe** | Queue is mutex+cv protected; `enqueue` is reentrant |
| Calling sync verb after `_async` without waiting | **Safe** | Sync verb's `_async().get()` enqueues behind the in-flight op; FIFO holds |
| Holding two futures from one `Client` | **Safe** | Both futures resolve in submission order |
| Destroying `Client` while a future is outstanding | **Unsafe** | The worker may be holding the packaged_task; destruction joins on the in-flight task but the future-holder sees a broken_promise if the worker exits without running |
| `Client` as a thread-local | **Unsafe** | The worker thread is a separate OS thread; thread_local on the constructor's thread doesn't extend to the worker |
| `Client` as a static / singleton | **Allowed** | Standard "static init / destroy order" caveats apply; prefer narrower lifetime |

The "destroying while a future is outstanding" case is worth elaborating:
the destructor waits for the worker's current task to finish before joining,
so any future the worker actually started will resolve normally. Futures
for queued-but-not-yet-started tasks will see the worker exit without
running them — the `std::packaged_task` destructor signals a broken promise,
and the holder gets `std::future_error{std::future_errc::broken_promise}` on
`.get()`. The fix is: don't destroy the `Client` while you still hold
outstanding futures from it. Either await them all first, or hold the
`Client` for the same lifetime as the futures' consumer.

## stop_token plumbing — planned

The §C2 handoff calls for cancellation via `std::stop_token` — every queued
op carries a stop_token; cancel = post stop, worker observes between chunk
boundaries (every 512 B for cmd 0x21 reads, every chunk for cmd 0x22
writes). **This is not in the v1 worker queue.** The current contract is:
once an op starts on the worker, it runs to completion (success, error, or
transport timeout from `ClientConfig::file_data_io_timeout`).

Workarounds today:

- For a "user cancelled the pull" UI, drop the future on the floor: the
  op continues on the worker but its result goes nowhere. Subsequent
  ops queue behind it normally.
- For a hung op, `ClientConfig::file_data_io_timeout` (default 30 s) is
  the upper bound on how long a single cmd 0x21 receive blocks the
  worker.
- For a wedged AP, only a physical replug recovers the firmware (per
  docs/34 §"Malformed-body daze"). The worker thread will eventually
  unblock once libusb returns its pipe error; the resulting future
  resolves with a `TransportTimeout` or `TransportError`.

When `stop_token` lands (planned for v1.5), the API addition will be:

- A new `*_async_stoppable` family that takes `std::stop_token`.
- The current `*_async` calls stay as the convenience form that doesn't
  plumb cancellation.
- The worker checks the token between chunks; on stop request it returns
  `Status` with `ErrorCode::Cancelled`.

The choice to defer is per the handoff's "ship the queue + a non-trivial
async op in the same commit" guidance — the Library panel will be the
first non-trivial async consumer, and stop_token will land in the same
batch as the cancel-button UX.

## Test coverage

The worker contract is pinned by
[`tests/unit/devices/ets/test_worker_queue.cpp`][tests]. Five cases:

[tests]: ../../tests/unit/devices/ets/test_worker_queue.cpp

1. **Destructor stops worker without hanging** — construct + destroy with
   no ops queued. Catch2 times out if the join deadlocks.
2. **`ls_async` returns a future that eventually resolves** — submit one
   async op, assert `wait_for(5s) == ready`.
3. **Sync `ls` routes through the worker queue** — confirms the
   `ls_async().get()` shim doesn't deadlock against its own worker.
4. **FIFO submission order** — three back-to-back submissions all
   resolve. The `LoopbackByteChannel` hands replies out in queued order
   so a non-FIFO worker would mis-pair requests and the test would
   time out.
5. **32-op stress** — exercises the cv-wakeup + lock-guard interaction
   under churn. Catches missed `notify_one` and wakeup-race bugs.

All five use the existing `LoopbackByteChannel` helper — no hardware,
no Frida, no live AP. They cover the contract, not the wire format
(which is covered by the per-cmd transport tests).

To run just these:

```bash
ctest --test-dir build --output-on-failure -R worker_queue
```

## Future work

What the v1 queue intentionally does not yet do:

- **Cancellation** — see *stop_token plumbing* above. Planned for v1.5
  alongside the Library panel cancel-button UX.
- **Priority lanes** — the §C2 handoff calls for a separate "fast lane"
  for `query_state` so that a long backup-all op doesn't block the
  status bar's ~1 Hz refresh. Today the status bar tick blocks behind
  bulk ops. The fix is a second worker thread for status-only ops with
  a separate mini-queue, OR a single queue with op-class priorities. Open
  design question.
- **Per-op timeout** — `ClientConfig` carries one `io_timeout` and one
  `file_data_io_timeout` that apply to all ops. A "this specific call
  must complete in 500 ms or fail" knob would simplify the GUI's
  status-bar refresh budget. Not currently in the API.
- **Cancellation between USB packets** — even with `stop_token`, the worker
  only observes the token between protocol-level chunks (a single libusb
  bulk transfer is atomic). Sub-packet cancellation requires a transport-
  layer change (a cancellable `IByteChannel`) and is out of scope.
- **Progress reporting** — the §C2 handoff calls for each op posting
  `(done_bytes, total_bytes_estimate)` to an `std::atomic<Progress>` the
  GUI polls per frame. Not yet implemented; the current contract is
  "futures resolve once when done, no intermediate signal." A
  `progress_token` parameter sibling to `stop_token` is the likely shape.
- **Interleaved ops** — the queue is strict FIFO and the channel is strict
  serial. There is no "preempt this read for a higher-priority write"
  affordance. This is a property of the AP3 transport, not a deficiency
  of the queue — interleaving at the worker would daze the device.

## Related

- [`docs/34-cobb-ap-as-tune-vault.md`][docs34] — the public capability doc
  for the COBB AP3 file-vault integration; the worker queue is the
  threading substrate under that capability's CLI + GUI surfaces
- [`docs/13-transport.md`](13-transport.md) — `IByteChannel` abstraction;
  the worker is the sole owner of the channel after `Client` construction
- [`docs/15-clean-room-engineering.md`](15-clean-room-engineering.md) —
  trademark / clean-room posture; type-name convention (`Client` lives in
  `st::devices::ets::`, not under a vendor name)
- `findings/handoffs/HANDOFF-from-analyst-2026-06-13-ap-browser-decisions.md`
  §C2 — strategic rationale for picking C2 (single worker + queue) over
  C1 (per-op thread) and C3 (coroutines)
- `src/devices/ets/include/st/devices/ets/client.hpp` — the public API
- `src/devices/ets/src/client.cpp` — `worker_loop()`, `enqueue<F>`, and
  the four `*_impl` methods that run on the worker
- `tests/unit/devices/ets/test_worker_queue.cpp` — the 5 contract tests
