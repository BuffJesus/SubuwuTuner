# 22 — Auto-update / `st::updater` (Phase 6)

> **Status:** design sketch. No implementation. Targets the v1.0 ship-blocker "Installer / codesigning / auto-update channel" in `docs/04-roadmap.md`. Implementation tracks Phase 6 polish, not earlier — the developer's iteration pain (zip-and-send between desktop and laptop) is meanwhile solved by file-sync, not by the in-tool updater.

## Why

Two pressure points motivate this work.

1. **Developer iteration loop.** Today: build on desktop, `zip` the `build/win-mingw/bin/` output, copy to laptop, unzip, test. Friction enough to discourage testing on the laptop. The in-tool updater plus a CI-driven release feed would let the laptop pull the latest desktop build with one click.
2. **User update channel.** Tuning tools must ship updates often (definition packs change, jurisdiction profiles evolve, bug fixes land). A "Help → Check for Updates" flow that downloads + verifies + restarts is table stakes for v1.0. Hand-rolled "download the new exe, manually replace, lose your settings" is not acceptable.

Both share the same pipeline: signed artifact in a known place, a check that compares versions, a verified download, an atomic swap, a restart. The developer-facing channel is just a different feed.

## Non-goals

- **Auto-updating definition packs.** Path-B distribution (`docs/17`) ships definitions out-of-band. The updater touches the SubuwuTuner binary + bundled assets only; definition-pack distribution is its own design (likely a separate `Help → Update Definition Packs` flow once Path B has a canonical pack-server URL).
- **Forced updates.** The user always decides whether to install an available update. No silent background install. No expiring "this version stops working on date X" behavior.
- **Differential updates / binary patching.** Full-binary swaps are simpler, easier to verify, and the binary is small enough (~50 MB target) that bandwidth isn't a concern. Differential updates are a future optimization.
- **Cross-version migration logic.** Configuration / project files must be forward-compatible by the project itself (see `.stune` schema versioning in `docs/21`). The updater does not run migrations.

## Architecture overview

```
   ┌──────────────────────────────────────┐
   │ GitHub Releases / CI artifact server │  ← source of truth: tagged releases
   └────────────┬─────────────────────────┘     for stable, CI artifacts for dev
                │ HTTPS GET (manifest.json + binary)
                ▼
   ┌──────────────────────────────────────┐
   │ st::updater (this module)            │
   │   - check(channel) → UpdateInfo      │
   │   - download(info) → staged path     │
   │   - verify(staged, manifest) → ok    │
   │   - apply(staged) → swap + restart   │
   └────────────┬─────────────────────────┘
                │
                ▼
   ┌──────────────────────────────────────┐
   │ ui::UpdateModal (Help → Check for…)  │
   │   - progress, version diff, release  │
   │     notes, confirm-to-install button │
   └──────────────────────────────────────┘
```

### Module layout (proposed)

```
src/updater/
   include/st/updater.hpp
   src/updater.cpp
   src/http_client.cpp         # thin wrapper, see "HTTP" below
   src/swap_windows.cpp        # platform-specific atomic-swap
   src/swap_posix.cpp          # rename(2)-based; trivial on Linux/macOS
tests/unit/updater/
   test_manifest.cpp           # manifest parse + version compare
   test_verify.cpp             # hash / signature verification
   test_swap_planner.cpp       # "what file moves where" — no I/O
```

No new third-party dependencies if possible. HTTP can ride on the existing fetch surface (`tools/defgen` already uses `urllib`; the C++ side may pull in `libcurl` or use OS-native APIs — see HTTP section below). Cryptography for signature verification should reuse whatever the brick-protection work in `docs/05` §4 picks for ECU-image signing (likely Ed25519 via libsodium or BoringSSL's stripped-down primitives), so the project has one cryptography seam, not two.

## Channels

| Channel | Source | Cadence | Audience |
|---|---|---|---|
| `stable` | GitHub Releases (tagged `v1.x.y`) | per release | End users (default) |
| `nightly` | CI artifact (every push to `main`) | per commit | Developers + opt-in testers |
| `dev` | Local file share / Syncthing (no manifest, just the binary) | continuous | Solo dev iterating; **out of scope for v1.0 — covered by file-sync, not this module** |

A user picks the channel in `Settings → Updates`. `stable` is the default; `nightly` is gated behind "I understand this is unstable" (one-line confirmation, not a EULA wall — the user is a knowledgeable adult who picked it on purpose). The `dev` channel is intentionally not handled by the updater; that's a file-sync problem with a file-sync solution.

Per-channel manifest URL:

- `stable` → `https://api.github.com/repos/BuffJesus/SubuwuTuner/releases/latest`
- `nightly` → `https://raw.githubusercontent.com/BuffJesus/SubuwuTuner/main/.github/nightly-manifest.json` (a CI job rewrites this on every push to `main`)

Both flow through the same `UpdateInfo` shape downstream so the UI doesn't care which channel produced it.

## API sketch

```cpp
namespace st::updater {

enum class Channel : std::uint8_t { Stable, Nightly };

struct UpdateInfo {
    Channel channel;
    std::string version;        // semver, e.g. "1.2.0" or "1.2.0-nightly+abc1234"
    std::string release_notes;  // markdown, surface in the modal
    std::string download_url;   // HTTPS URL to the platform-specific binary archive
    std::string sha256;         // expected hash of the downloaded archive
    std::optional<std::string> signature;  // base64 Ed25519 sig over the sha256
    std::uint64_t size_bytes;
};

// Hit the channel manifest, parse, compare against st::Version::string().
// Result is nullopt when the running build is already up-to-date or newer.
// Errors: network, parse, signature.
[[nodiscard]] Result<std::optional<UpdateInfo>> check(Channel channel,
                                                      std::chrono::milliseconds timeout =
                                                          std::chrono::seconds{15});

// Stream the download to a temp path. Calls `progress(bytes_done, bytes_total)`
// every ~64 KB. Verifies sha256 + signature before returning. The returned
// path is in a per-user staging dir; caller owns cleanup if apply() doesn't
// consume it.
using DownloadProgressFn = std::function<void(std::uint64_t, std::uint64_t)>;
[[nodiscard]] Result<std::filesystem::path> download(UpdateInfo const &info,
                                                     DownloadProgressFn progress = nullptr,
                                                     std::atomic<bool> const *cancel = nullptr);

// Replace the running binary's install directory with the staged contents,
// then schedule a restart (re-exec the new binary). Platform-specific —
// see "Swap mechanics" below. On Windows, atomic swap requires either a
// helper-process pattern or a shim launcher; on POSIX, rename(2) into
// place is enough.
//
// Returns Ok on a successful schedule (process will exit shortly).
// Returns failure if the swap could not be staged (e.g. write permission
// on the install dir; the caller surfaces this and tells the user to
// re-run from an elevated session or reinstall).
[[nodiscard]] Status apply(std::filesystem::path const &staged_archive);

}  // namespace st::updater
```

## Manifest format

GitHub Releases provides JSON via the API; the project's CI nightly manifest is a smaller hand-rolled JSON written by `tools/build/emit_nightly_manifest.py` (TBD). One canonical shape parsed by `st::updater`:

```json
{
  "version": "1.2.0",
  "channel": "stable",
  "release_notes": "## v1.2.0\n- Foo\n- Bar\n",
  "assets": [
    {
      "platform": "win-x64",
      "url": "https://github.com/BuffJesus/SubuwuTuner/releases/download/v1.2.0/SubuwuTuner-1.2.0-win-x64.zip",
      "sha256": "deadbeef...",
      "signature": "base64-ed25519-over-sha256",
      "size_bytes": 52428800
    },
    {
      "platform": "macos-arm64",
      "url": "...",
      "sha256": "...",
      "signature": "...",
      "size_bytes": 50331648
    },
    {
      "platform": "linux-x64",
      "url": "...",
      "sha256": "...",
      "signature": "...",
      "size_bytes": 49283072
    }
  ]
}
```

The platform string follows GoLang's `${GOOS}-${GOARCH}` convention because it's unambiguous and well-known. `st::updater::check` selects the asset matching the current build's platform; mismatch is surfaced as "no asset for your platform" (with a hint to download manually).

## HTTP

Decision pending. Options ranked by complexity:

1. **Embed libcurl.** Battle-tested, handles HTTPS / redirects / certificate bundles. Adds ~500 KB to the binary. Already vcpkg-available, fetch via FetchContent fine.
2. **Use OS-native APIs.** WinHTTP on Windows, NSURLSession on macOS, libcurl on Linux. More platform code, no third-party dependency. Probably right for v1.0 if we can stomach the per-OS code.
3. **Shell out to `curl` / `wget`.** Avoids a library dependency entirely, but adds an installed-tool prerequisite that's not present on every Windows host. Skip.

Defer the call until implementation; the API surface (`download(info, ...)`) doesn't reveal the choice.

## Signature verification

Required, not optional. Stable-channel binaries are signed with the project's release key (Ed25519); the public key is **compiled into the binary** at build time so a network-level attacker can't swap both the binary and the verification key. Channel manifests carry the signature; verification happens before `download()` returns. Failure surfaces as a hard error — never "warn, install anyway."

Nightly channel uses the same key. If the project's key is ever compromised, we ship a "panic update" through the still-trusted key that rotates the embedded public key in the next release, then rotate the signing key.

Codesigning (Windows Authenticode / macOS notarization) is a separate concern from the updater's signature verification. Authenticode mostly stops Windows from displaying the SmartScreen warning; SubuwuTuner's updater verifies regardless. macOS notarization is what gets Gatekeeper out of the user's way; same rule applies — `st::updater` verifies independently.

## Swap mechanics

The hard problem on Windows: the OS locks a running EXE. Three options:

1. **Helper-process pattern.** The running app downloads, spawns a small `update-applier.exe` with the new install staged, then exits. The helper waits for the parent to fully exit, replaces the install directory, then restarts the main binary. **Recommended for v1.0** — bounded complexity, well-understood, ships in many tools (including Slack, Discord, Steam).
2. **Squirrel.Windows-style versioned install.** Each version lives in `\app-1.2.0\`, a shim launcher in the parent dir always launches the latest. Allows atomic swap without process gymnastics. Heavier upfront design; sleeker user experience.
3. **MoveFileEx with `MOVEFILE_DELAY_UNTIL_REBOOT`.** Schedules the swap for next boot. Simple but the user has to manually reboot. Bad UX. Reject.

POSIX has `rename(2)` which is atomic within a filesystem; the swap is a one-liner. macOS app bundles complicate this slightly (the .app is a directory; replacing it atomically requires a temp-then-rename of the whole bundle), but standard practice.

The seam `apply(staged)` hides all of this from callers.

## UI flow

```
┌─────────────────────────────────────────────────┐
│ SubuwuTuner Update Available                    │
│ ─────────────────────────────────────────────── │
│ Current: 1.0.0                                  │
│ Latest:  1.0.2  (stable)                        │
│ Size:    51.3 MB                                │
│                                                 │
│ ## Release notes                                │
│ - Cancellation invariants closed for v1.0       │
│ - Address gate now ships                        │
│ - Two-finger pan in the table editor            │
│ ─────────────────────────────────────────────── │
│ [Install now]  [Later]  [Skip this version]     │
└─────────────────────────────────────────────────┘
```

`Install now` runs `download` (progress bar replaces the modal body) then `apply` and exits. `Later` defers the check to the next launch. `Skip this version` records the version in user settings so the modal does not re-prompt until something newer arrives.

The check itself runs on a worker thread; the main UI thread polls the result via the `Result<std::optional<UpdateInfo>>` future. No blocking the UI on network I/O ever.

`Help → Check for Updates` re-runs the check manually. By default the check runs once at startup, throttled to "no more than once per 24 hours per channel."

## Open questions

1. **Settings persistence.** Where do "channel preference" and "skipped versions" live? Probably `%LOCALAPPDATA%/SubuwuTuner/updater.toml` on Windows, mirrored on POSIX. Schema is two fields and an array.
2. **Per-user vs per-machine install.** A per-user install lets `apply()` write without admin elevation. A per-machine install needs an elevation prompt and a UAC handler — possibly via the helper process. v1.0 ships per-user; per-machine is an installer-time choice (`docs/04` row "Installer / codesigning / auto-update channel" stays the parent ticket).
3. **Telemetry.** The check itself sends a `User-Agent` plus an opaque-version string to GitHub. No analytics, no identifiers beyond what the HTTP layer reveals. Document this in the Privacy Notice (TBD).
4. **Rollback.** If a `1.0.2` install is broken, users today reinstall by hand. A `--rollback` CLI flag that restores the previous install (kept in `app-prev/`) is a nice-to-have, not v1.0.
5. **Definition-pack channel.** Out of scope here (see Non-goals), but the same manifest + verify + download infrastructure could serve a future `st::pack_updater` for Path-B definition distribution. Worth keeping the `st::updater` types generic enough to reuse.

## Sequencing

This module sits in Phase 6 of `docs/04-roadmap.md`. Reasonable order once it starts:

1. CI nightly-manifest emit script + signed-binary build job. Without these the updater has nothing to consume.
2. `st::updater::check` + manifest parsing + tests (no I/O — feed canned JSON to the parser).
3. `st::updater::download` + verification + tests against a local HTTP server fixture.
4. `apply()` per platform — Windows helper-process first (highest-friction case), then POSIX.
5. GUI flow.
6. Codesigning + Authenticode + macOS notarization. **These gate the v1.0 user release** but the updater can be developed and tested before them.

The developer-iteration pain that motivated this doc is solved meanwhile by file-sync between desktop and laptop (`tools/sync-to-laptop.ps1` candidate; Syncthing for the no-code path). Don't block the iteration loop on landing the updater.
