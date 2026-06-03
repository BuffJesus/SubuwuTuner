# PyInstaller spec for `defgen` — closes docs/04 §Pre-1.0 ship blocker #7.
# Build a single-file standalone executable so end users don't need a
# Python environment installed. The output `dist/defgen.exe` (Windows) /
# `dist/defgen` (POSIX) ships in the release installer at
# `tools/defgen.exe` next to subuwutuner-cli.
#
# Build:
#   cd tools/defgen
#   pip install pyinstaller
#   pyinstaller defgen.spec --clean
#
# Resulting binary is self-contained — no .py / no interpreter on the
# host. Tested in this configuration:
#   - Windows: Python 3.12, PyInstaller 6.x → dist/defgen.exe (~10-15 MB)
#   - macOS:   Python 3.12, PyInstaller 6.x → dist/defgen
#   - Linux:   Python 3.12, PyInstaller 6.x → dist/defgen
#
# What's bundled:
#   * defgen.py — entry point
#   * descriptors.py — RomRaider tag handlers
#   * data/ — per-platform mapping YAMLs (loaded at runtime via files())
#
# What's NOT bundled (deliberate):
#   * categorize.py, cousin_seed.py, apply_offset.py — operator-side
#     helpers used during definition-pack development, not by end users
#     running rom→def conversion. Ship as plain .py only when the
#     analyst-mode prompt asks for them; the frozen binary is the
#     end-user surface.

# -*- mode: python ; coding: utf-8 -*-

block_cipher = None

a = Analysis(
    ['defgen.py'],
    pathex=[],
    binaries=[],
    datas=[
        # Per-platform mapping YAMLs — defgen reads these at runtime
        # via importlib.resources.files(). Bundle the directory so the
        # frozen binary finds them next to itself.
        ('data', 'data'),
    ],
    hiddenimports=[],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[
        # PyInstaller drags these in by default for any project with
        # numeric ops; defgen does not use them. Excluding saves ~20 MB
        # on the resulting binary.
        'numpy',
        'pandas',
        'matplotlib',
        'scipy',
        'PIL',
        'tkinter',
    ],
    win_no_prefer_redirects=False,
    win_private_assemblies=False,
    cipher=block_cipher,
    noarchive=False,
)

pyz = PYZ(a.pure, a.zipped_data, cipher=block_cipher)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.zipfiles,
    a.datas,
    [],
    name='defgen',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=True,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
