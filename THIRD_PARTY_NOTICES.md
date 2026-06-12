# Third-party notices

SubuwuTuner is licensed under [Apache 2.0](LICENSE). This file lists the third-party software it links against and the third-party references it has studied as data sources, per `docs/15-clean-room-engineering.md` §11.

## Linked dependencies

All of the following are pulled in via CMake `FetchContent` and statically linked into the SubuwuTuner binaries. Each is distributed under a license compatible with Apache 2.0.

| Project | Version | License | Used for | Upstream |
|---|---|---|---|---|
| Catch2 | v3 | BSL-1.0 | unit tests (`tests/`) | https://github.com/catchorg/Catch2 |
| tl::expected | latest | CC0-1.0 | `Result<T>` fallback when `<expected>` is unavailable | https://github.com/TartanLlama/expected |
| tomlplusplus | v3.4 | MIT | TOML parsing (`st::defs`) | https://github.com/marzer/tomlplusplus |
| GLFW | 3.4 | Zlib | window/input (`subuwutuner-gui`) | https://github.com/glfw/glfw |
| Dear ImGui | v1.91 | MIT | immediate-mode GUI | https://github.com/ocornut/imgui |
| ImPlot | latest | MIT | plotting widgets (heatmap, histogram) | https://github.com/epezent/implot |
| nativefiledialog-extended | latest | Zlib | native Open/Save dialogs | https://github.com/btzy/nativefiledialog-extended |
| libusb-1.0 (via libusb-cmake) | v1.0.27-0 | LGPL-2.1 + linking exception | USB byte channel for the AP3 file vault (default-on; build with `-DST_ENABLE_AP3=OFF` to omit) | https://github.com/libusb/libusb-cmake |
| tiny-AES-c | master (vendored) | Public domain (Unlicense) | AES-256-CTR primitive for the optional `.ptm` cipher chain (gated; ON only when `-DST_ENABLE_COBB_AP_CIPHER=ON`). Vendored under `src/devices/ap3/third_party/tiny_aes/`. | https://github.com/kokke/tiny-AES-c |
| bzip2 1.0.8 (decompress-only) | 1.0.8 (vendored, trimmed) | bzip2 license (BSD-style) | bzip2 inflation for the optional `.ptm` cipher chain layer 4. Vendored under `src/devices/ap3/third_party/bzip2_dec/` with compress-side sources (`compress.c`, `blocksort.c`) dropped per `specs/cobb-ap3-tier3-dep-survey.md`; references from `bzlib.c` resolved via `compress_stubs.c` no-op shims. | https://sourceware.org/pub/bzip2/ |

Full license texts are bundled with the source of each dependency under `_deps/<name>-src/` after a CMake configure.

## Build-time tools

These are not linked, only invoked during build:

- **CMake** (≥ 3.28, BSD 3-Clause)
- **Ninja** (Apache 2.0)
- **clang-format** (Apache 2.0 with LLVM Exceptions) — formatting CI advisory job
- **clang-tidy** (Apache 2.0 with LLVM Exceptions) — static analysis
- **Python** (≥ 3.12, PSF License) — for `tools/defgen/` and `tools/defgen/loggergen.py`

## Studied references (data sources)

Per `docs/15-clean-room-engineering.md` §11, the project records every external reference it consulted to derive *facts* (addresses, scaling formulas, protocol bytes, frame layouts). These references were not copied; their facts were extracted under the clean-room methodology in `docs/15`.

### Standards

The cleanest possible provenance — preferred over any competitor's interpretation:

- **ISO 14229-1** — UDS application layer
- **ISO 15765-2** — CAN-TP (network/transport for UDS over CAN)
- **ISO 14230** — KWP2000 (K-Line application protocol underlying Subaru SSM on VA-era cars)
- **ISO 11898** — CAN physical/data link
- **SAE J2534** — Pass-Thru API (Tactrix etc.)
- **SAE J1979** — OBD-II diagnostic services
- **SAE J2012** — DTC numbering

### Open-source references

- **RomRaider** (GPL-2.0, https://github.com/RomRaider/RomRaider) — public protocol documentation and community ECU definition XML. `tools/defgen/` consumes the XML for fact extraction; the Java source was not used.
- **Merp's SubaruDefs** (https://github.com/Merp/SubaruDefs) — community-curated RomRaider XML, source of the older-Subaru packs in `definitions/{impreza,forester,legacy,liberty,outback,baja,tribeca,exiga}/`.

### Third-party hardware interoperability

- **COBB AccessPort V3 USB protocol** — reverse-engineered (Capability A: file vault, wire format + dispatcher + FileInfo2 layout, no cipher source incorporated by default) for the `st::devices::ap3` integration. Provenance and the analyst-side audit trail are documented in `D:\Subuwu\specs\references\cobb-ap3-usb-protocol.md` (private analyst spec) and the public-side gating model in `docs/34-cobb-ap-as-tune-vault.md`. The COBB binaries (APManager.exe and the AP firmware OTA `.img` archives) were studied as facts-only references under the clean-room methodology in `docs/15` §6 and the §1201 posture in §12. No binary source, decompile output, function-symbol names, or authored prose was incorporated into SubuwuTuner. The optional gated `.ptm` cipher (`ST_ENABLE_COBB_AP_CIPHER`) does not ship in the default public build.

### Community discussion

- Public forum threads and posts on `romraider.com`, `nasioc.com`, and similar Subaru tuning communities — used as cross-checks for protocol observations and seed/key constant verification.

## Bundled calibration data

The public repository carries definition packs for older Subarus (Impreza, Forester, Legacy, Liberty, Outback, Baja, Tribeca, Exiga) generated by `tools/defgen/` from Merp's SubaruDefs. These are community-sourced and inherit Merp's posture; their TOML files preserve short functional names from the source XML (e.g., "Boost Target") and strip long descriptive prose per the clean-room rule in `docs/15` §2. The shared `definitions/pids.toml` (SSM datalogger PIDs + switches) and `definitions/ecuparams/` (per-CID extended-PID fragments) are similarly Merp-derived.

## NOT shipped

The public repository does **not** bundle calibration packs for the **VA WRX (2015–2021)** or **VB WRX (2022+)** platforms. See `docs/17-data-distribution-policy.md` for the policy and `docs/install.md` for the user-side workflow to obtain or generate VA/VB packs independently.

## Future contributions

Per `docs/17-data-distribution-policy.md` §4, future community-contributed first-party calibration packs must satisfy both the copyright filter (clean-room methodology in `docs/15`) and the §1201 filter (no upstream access-control circumvention against any commercial tuning tool). New packs land here with their own provenance citation.

## Bundled fonts

The GUI is designed to load **Inter** (sans-serif, UI body) and **JetBrains Mono** (monospace, code/hex), each licensed under the SIL Open Font License v1.1 (OFL-1.1). When the source tree carries the TTF binaries (typically delivered in the release installer at `assets/fonts/`), this license governs their redistribution. When no font is bundled, the GUI falls back to ImGui's default font and these licenses do not apply.

- **Inter** — Copyright (c) The Inter Project Authors (https://github.com/rsms/inter)
- **JetBrains Mono** — Copyright (c) JetBrains s.r.o. (https://github.com/JetBrains/JetBrainsMono)

Both fonts ship unmodified.

### SIL Open Font License, Version 1.1

```
SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007

PREAMBLE
The goals of the Open Font License (OFL) are to stimulate worldwide
development of collaborative font projects, to support the font creation
efforts of academic and linguistic communities, and to provide a free and
open framework in which fonts may be shared and improved in partnership
with others.

The OFL allows the licensed fonts to be used, studied, modified and
redistributed freely as long as they are not sold by themselves. The
fonts, including any derivative works, can be bundled, embedded,
redistributed and/or sold with any software provided that any reserved
names are not used by derivative works. The fonts and derivatives,
however, cannot be released under any other type of license. The
requirement for fonts to remain under this license does not apply to any
document created using the fonts or their derivatives.

DEFINITIONS
"Font Software" refers to the set of files released by the Copyright
Holder(s) under this license and clearly marked as such. This may
include source files, build scripts and documentation.

"Reserved Font Name" refers to any names specified as such after the
copyright statement(s).

"Original Version" refers to the collection of Font Software components as
distributed by the Copyright Holder(s).

"Modified Version" refers to any derivative made by adding to, deleting,
or substituting -- in part or in whole -- any of the components of the
Original Version, by changing formats or by porting the Font Software to a
new environment.

"Author" refers to any designer, engineer, programmer, technical
writer or other person who contributed to the Font Software.

PERMISSION & CONDITIONS
Permission is hereby granted, free of charge, to any person obtaining
a copy of the Font Software, to use, study, copy, merge, embed, modify,
redistribute, and sell modified and unmodified copies of the Font
Software, subject to the following conditions:

1) Neither the Font Software nor any of its individual components,
in Original or Modified Versions, may be sold by itself.

2) Original or Modified Versions of the Font Software may be bundled,
redistributed and/or sold with any software, provided that each copy
contains the above copyright notice and this license. These can be
included either as stand-alone text files, human-readable headers or
in the appropriate machine-readable metadata fields within text or
binary files as long as those fields can be easily viewed by the user.

3) No Modified Version of the Font Software may use the Reserved Font
Name(s) unless explicit written permission is granted by the corresponding
Copyright Holder. This restriction only applies to the primary font name as
presented to the users.

4) The name(s) of the Copyright Holder(s) or the Author(s) of the Font
Software shall not be used to promote, endorse or advertise any
Modified Version, except to acknowledge the contribution(s) of the
Copyright Holder(s) and the Author(s) or with their explicit written
permission.

5) The Font Software, modified or unmodified, in part or in whole,
must be distributed entirely under this license, and must not be
distributed under any other license. The requirement for fonts to
remain under this license does not apply to any document created
using the Font Software.

TERMINATION
This license becomes null and void if any of the above conditions are
not met.

DISCLAIMER
THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE
COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL
DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM
OTHER DEALINGS IN THE FONT SOFTWARE.
```
