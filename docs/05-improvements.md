# 05 — What Makes SubuwuTuner Different

SubuwuTuner is a comprehensive Subaru tuning suite designed to be the tool the community would build for itself if it started from scratch today. The improvements below are the places where being a native, modern C++ codebase, open from the start, gives us a structural edge over existing tools.

## 1. Native startup, memory, and binary size

JVM-based tuning tools pay a real cost in cold-start time and idle RAM. Targets for SubuwuTuner v1.0:

| Metric | SubuwuTuner target |
|---|---|
| Cold start to interactive window | < 800 ms |
| Idle RAM after opening a project | < 150 MB |
| Installer size | < 60 MB |

## 2. Headless / scriptable mode

`subuwutuner-cli` shares the same domain model as the GUI: open a project, dump a map to CSV, batch-apply a patch across N ROMs, run a flash from a recipe file. This unlocks CI for tune authors, fleet workflows for shops, and reproducible pipelines for dyno operators — none of which the existing GUI-only Subaru tuning tools support.

## 3. Definition format as source-of-truth, version-controlled

`*.stune` projects are git-friendly. Definitions are TOML. Tune development becomes diffable, mergeable, reviewable — bring tune-shop workflows into the same world the rest of software lives in.

## 4. Brick protection — formalized, testable

Brick protection is a first-class subsystem with a written threat model:

- Recovery shim is installed and **verified by reading it back** before any subsequent write
- Recovery shim sits in a separate flash bank or in a region the bootloader is guaranteed to read
- Every flash operation publishes a tamper-evident manifest (BLAKE3 over sector hashes) that the user can keep
- If the host machine dies mid-flash, the next boot of SubuwuTuner offers to resume from the manifest

The whole subsystem is bench-tested on real ECUs as part of CI — junkyard ECUs on a bench harness, automated. See `08-testing-strategy.md`.

## 5. Real-time datalogging quality

- Sustained 100 Hz on supported adapters
- Lock-free sample ring buffer; UI samples a snapshot, never blocks the I/O thread
- Per-PID timestamps from the adapter (where available), not wall-clock interpolation
- Replay format is FlatBuffers — zero-copy decode for instant scrubbing

## 6. Plugin safety

The host-side compilation and transform step is sandboxed (Lua, no `io`, no `os`, no FFI), so a malicious community-shared pack cannot exfiltrate your project files or modify them outside the editor's review flow. Any ECU-bytecode patch is reviewed in a preview pane before it ever touches a ROM.

## 6a. First-class, open auto-tune

The Subaru community's options for auto-tune are dated, GUI-only, or paid-and-closed. SubuwuTuner ships first-party MAF auto-tune and knock-based ignition pull in v1.1, with closed-loop trim integration, boost auto-trim, and idle trim in v1.2. CLI-first so a driver can run `subuwutuner-cli autotune maf --log run.csv --project mytune.stune` between drives, or so a dyno operator can batch-process a day's worth of logs. Engine-safety linting runs over every proposal regardless of jurisdiction profile. See `docs/12-auto-tuning.md`.

## 6b. Jurisdiction profiles instead of paternalism

The user picks a configurable **jurisdiction profile** on first run (Alberta-CA, California-US, EU-roadworthy, motorsport-only, etc.). The default is `motorsport-only` — least restrictive. Each profile decides whether emissions-flagged edits draw a silent badge, a warning, or a confirmation. Engine-safety warnings (AFR way off, dangerous timing) stay blocking in every profile. See `06-legal-ethics.md`.

## 7. Better gauges, fewer dependencies

A first-party 2D gauge widget set built on ImGui's `DrawList` (direct triangles + lines, no widget overhead), plus **ImPlot** for log plotting and live datalog charts — same author as ImGui, 100k-point real-time series without dropping frames. Goal: 60 fps on integrated graphics. No WebGL, no embedded browsers.

## 8. Cross-platform on day one

We ship CI-tested binaries for **Windows x64, macOS arm64+x64 universal, Linux x64, Linux arm64** from day one, with the same feature set. No second-class platforms.

## 9. Native adapter drivers with proper backpressure

Adapter support (Tactrix, OBDLink, ELM327, OBDX Pro) is built on a coroutine-based AT-command framework where every command has a typed response, a timeout, and a retry policy declared in code — easier to extend with new adapter quirks than a hand-rolled state machine.

## 10. Telemetry posture

**Zero analytics, zero phone-home.** Crash reporting is opt-in, contains no project content, and is sent to a self-hostable endpoint.

---

## Where we are conservative

- **Vehicle coverage at v1.0.** VA + VB WRX MT only because that's what we can brick-test on the Phase-4 bench rig. Architecture is multi-platform from day one (see `02-architecture.md`); expansion order is in `04-roadmap.md`.
- **No defeat-preset content shipped first-party.** The tool exposes what the ECU exposes; we don't ship pre-built "delete" calibrations.
- **No write path on unsupported adapters.** Flashing only goes over verified, documented J2534-class paths until proven safe elsewhere.
