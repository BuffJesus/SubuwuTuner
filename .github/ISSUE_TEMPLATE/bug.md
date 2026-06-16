---
name: Bug report
about: Something is wrong. Crashes, incorrect behavior, broken UI.
title: '[bug] '
labels: bug
---

<!-- For security issues (crashes that could be exploited, anything affecting flash safety), do NOT open a public issue. See SECURITY.md. -->

## What happened

<!-- One sentence describing the wrong behavior. -->

## What you expected

<!-- One sentence describing the right behavior. -->

## Steps to reproduce

1.
2.
3.

## Environment

- **SubuwuTuner version / commit:** <!-- `subuwutuner-cli --version` or git SHA -->
- **OS:** <!-- Windows 11 / Ubuntu 24.04 / macOS 14 / etc. -->
- **Build:** <!-- mingw / msvc / clang / etc. - from CMakePresets.json -->
- **Cipher flag:** <!-- ST_ENABLE_COBB_AP_CIPHER=ON or OFF -->
- **Hardware:** <!-- OBDX Pro VX on COM5 / COBB AP V3 / J2534 / none -->

## Output

<!-- If the CLI: paste the failing command + the stderr/stdout. -->

<!-- If the GUI: a screenshot of the panel + the toast text. -->

<!-- If you ran `subuwutuner-cli doctor`, paste its output. -->

## Already tried

<!-- Did you try `subuwutuner-cli doctor`? Restarting? Reinstalling? Rebuilding without optimizations? -->

## ROM / project context (if applicable)

<!-- Don't paste full ROM bytes. CID (e.g. LF79103P), pack id, project size are useful. If a private fixture is needed to reproduce, see tests/private/README.md for the gitignored test path. -->
