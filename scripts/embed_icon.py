#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
#
# Regenerate src/ui/src/icon_data.hpp from assets/icon.png.
#
# Run after changing the source icon:
#     python scripts/embed_icon.py
#
# The header is compiled into subuwutuner-gui and used to set the GLFW
# window title-bar icon at startup (glfwSetWindowIcon). The Windows
# Explorer / taskbar EXE icon is set separately via assets/icon.ico +
# src/ui/subuwutuner.rc + windres (CMake wires this on Windows builds).
#
# Both paths originate from the same assets/icon.png — keep them in sync
# by re-running this script and re-running the Pillow snippet that
# regenerates icon.ico (see the original add of this icon, 2026-05-23).

from pathlib import Path

from PIL import Image

REPO = Path(__file__).resolve().parent.parent
PNG = REPO / "assets" / "icon.png"
OUT = REPO / "src" / "ui" / "src" / "icon_data.hpp"
ICO = REPO / "assets" / "icon.ico"

SIZE = 64  # px, square — GLFW picks the best match


def main() -> None:
    src = Image.open(PNG).convert("RGBA")
    img = src.resize((SIZE, SIZE), Image.LANCZOS)
    data = img.tobytes()
    assert len(data) == SIZE * SIZE * 4

    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i : i + 16]
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")

    OUT.write_text(
        f"""// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Auto-generated from assets/icon.png by scripts/embed_icon.py.
// DO NOT EDIT BY HAND — regenerate if the source PNG changes:
//   python scripts/embed_icon.py

#ifndef ST_UI_ICON_DATA_HPP
#define ST_UI_ICON_DATA_HPP

#include <cstddef>

namespace st::ui::icon {{

inline constexpr int kWidth = {SIZE};
inline constexpr int kHeight = {SIZE};

inline constexpr unsigned char kRgba[{len(data)}] = {{
{chr(10).join(lines)}
}};

}} // namespace st::ui::icon

#endif // ST_UI_ICON_DATA_HPP
""",
        encoding="utf-8",
    )
    print(f"wrote {OUT} ({OUT.stat().st_size:,} bytes)")

    # Multi-resolution Windows .ico for the EXE-side icon (via subuwutuner.rc).
    ico_sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
    src.save(ICO, format="ICO", sizes=ico_sizes)
    print(f"wrote {ICO} ({ICO.stat().st_size:,} bytes)")


if __name__ == "__main__":
    main()
