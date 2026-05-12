# 05 — Improvements Over Atlas

Porting is not the goal; the goal is a tool that is better than Atlas where being better is technically achievable. We pick our improvements where C++ + modern tooling gives a structural edge, not where we'd just be re-skinning.

## 1. Startup, memory, and binary size

Atlas pays the JVM tax: cold start measured in seconds, baseline RAM in the hundreds of megabytes. Target for SubaruTuner v1:

| Metric | Atlas (observed/typical) | SubaruTuner target |
|---|---|---|
| Cold start to interactive window | 4–8 s | < 800 ms |
| Idle RAM after opening a project | 400–800 MB | < 150 MB |
| Installer size | ~250 MB (incl. JRE) | < 60 MB |

## 2. Headless / scriptable mode

Atlas is GUI-only. SubaruTuner ships **`subarutuner-cli`** sharing the same domain model: open a project, dump a map to CSV, batch-apply a patch across N ROMs, run a flash from a recipe file. This unlocks CI for tune authors, fleet workflows for shops, and pipelines for dyno operators.

## 3. Definition format as source-of-truth, version-controlled

`*.stune` projects are git-friendly. Definitions are TOML. Tune development becomes diffable, mergeable, reviewable — bring tune-shop workflows into the same world the rest of software lives in.

## 4. Brick protection — formalized, testable

Atlas claims "custom brick protection that far exceeds OEM recovery." We make this a first-class subsystem with a written threat model:

- Recovery shim is installed and **verified by reading it back** before any subsequent write
- Recovery shim is in a separate flash bank or in a region the bootloader is guaranteed to read
- Every flash operation publishes a tamper-evident manifest (BLAKE3 over sector hashes) that the user can keep
- If the host machine dies mid-flash, the next boot of SubaruTuner offers to resume from the manifest

The whole subsystem is bench-tested on real ECUs as part of CI — junkyard ECUs on a bench harness, automated. See `08-testing-strategy.md`.

## 5. Real-time datalogging quality

- Sustained 100 Hz on supported adapters (Atlas typically advertises ~50 Hz)
- Lock-free sample ring buffer; UI samples a snapshot, never blocks the I/O thread
- Per-PID timestamps from the adapter (where available), not wall-clock interpolation
- Replay format is FlatBuffers — zero-copy decode for instant scrubbing

## 6. Plugin safety

Atlas's "node graph custom features" execute on the ECU. We additionally sandbox the **host-side** compilation/transform step (Lua, no I/O), so a malicious community-shared pack cannot exfiltrate your project files or modify them outside the editor's review flow. The bytecode patch is reviewed in a preview pane before it ever touches a ROM.

## 6a. First-class, open auto-tune

The Subaru community's options for auto-tune are dated (RomRaider's MAF tuner), absent (EcuFlash, Atlas), or paid-and-closed (Cobb, EcuTek). SubaruTuner ships first-party MAF auto-tune and knock-based ignition pull in v1.1, with closed-loop trim integration, boost auto-trim, and idle trim in v1.2. CLI-first so a driver can run `subarutuner-cli autotune maf --log run.csv --project mytune.stune` between drives, or so a dyno operator can batch-process a day's worth of logs. Engine-safety linting runs over every proposal regardless of jurisdiction profile. See `docs/12-auto-tuning.md`.

## 6b. Jurisdiction profiles instead of paternalism

Atlas refuses outright to assist with emissions-equipment edits. SubaruTuner replaces that with a configurable **jurisdiction profile** picked on first run (Alberta-CA, California-US, EU-roadworthy, motorsport-only, etc.). The default is `motorsport-only` — least restrictive. Each profile tunes whether emissions-flagged edits draw a silent badge, a warning, or a confirmation. Engine-safety warnings (AFR way off, dangerous timing) stay blocking in every profile. See `06-legal-ethics.md`.

## 7. Better gauges, fewer dependencies

A first-party 2D gauge widget set built on QPainter, plus a QtCharts/QCustomPlot pipeline for log plotting. Goal: 60 fps on integrated graphics. No WebGL, no embedded browsers.

## 8. Cross-platform on day one

Atlas does support Mac and Linux, but the rough edges are real (font rendering, file dialogs, USB permissions). We ship CI-tested binaries for **Windows x64, macOS arm64+x64 universal, Linux x64, Linux arm64** from day one, with the same feature set.

## 9. Native ELM/OBDLink with proper backpressure

The Atlas README highlights a "natively-written driver." Ours is built on a coroutine-based AT-command framework where every command has a typed response, a timeout, and a retry policy declared in code — easier to extend with new adapter quirks than a hand-rolled state machine.

## 10. Telemetry posture

Atlas's posture isn't a problem for us, but we go further: **zero analytics, zero phone-home**. Crash reporting is opt-in, contains no project content, and is sent to a self-hostable endpoint.

---

## Things we will NOT try to beat Atlas on

- Vehicle coverage in v1 — same VA + VB scope
- Marketing polish / community size — they have a head start
- The visual node-graph UX itself — we copy the *idea* but keep our node editor minimal
