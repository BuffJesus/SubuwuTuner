# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors
"""Smoke tests for tools/build_definition_archives.py."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path


_THIS = Path(__file__).resolve()
_TOOL = _THIS.parent.parent / "build_definition_archives.py"
_spec = importlib.util.spec_from_file_location("build_definition_archives", _TOOL)
assert _spec is not None and _spec.loader is not None
build_def_arch = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = build_def_arch
_spec.loader.exec_module(build_def_arch)


def _make_fake_defs(root: Path) -> None:
    """Stand up a small fake definitions tree under `root`."""
    (root / "impreza").mkdir()
    (root / "impreza" / "a1.toml").write_text("# pack a1\n[pack]\nid = \"a1\"\n", encoding="utf-8")
    (root / "impreza" / "a2.toml").write_text("# pack a2\n[pack]\nid = \"a2\"\n", encoding="utf-8")
    (root / "legacy").mkdir()
    (root / "legacy" / "b1.toml").write_text("# pack b1\n[pack]\nid = \"b1\"\n", encoding="utf-8")
    # Empty subdir should be skipped (no files).
    (root / "empty_platform").mkdir()
    # Syncthing marker should be skipped.
    (root / ".stfolder").mkdir()
    (root / ".stfolder" / "syncthing-folder-test.txt").write_text("", encoding="utf-8")


class TestListPlatformDirs(unittest.TestCase):
    def test_skips_empty_dirs_and_syncthing_marker(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _make_fake_defs(root)
            dirs = build_def_arch.list_platform_dirs(root)
            names = sorted(d.name for d in dirs)
            self.assertEqual(names, ["impreza", "legacy"])


class TestBuildArchive(unittest.TestCase):
    def test_zip_contains_every_file(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _make_fake_defs(root)
            out = root / "_archives" / "impreza.zip"
            n, raw, zsize = build_def_arch.build_archive(root / "impreza", out)
            self.assertEqual(n, 2)
            self.assertGreater(raw, 0)
            self.assertTrue(out.is_file())
            with zipfile.ZipFile(out) as z:
                self.assertEqual(sorted(z.namelist()), ["a1.toml", "a2.toml"])
                self.assertIn(b'id = "a1"', z.read("a1.toml"))


class TestBuildCommand(unittest.TestCase):
    def test_builds_all_platforms(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _make_fake_defs(root)
            rc = build_def_arch.main(["--defs-dir", str(root)])
            self.assertEqual(rc, 0)
            archive_dir = root / "_archives"
            self.assertTrue((archive_dir / "impreza.zip").is_file())
            self.assertTrue((archive_dir / "legacy.zip").is_file())
            # Empty platform should not have produced a zip.
            self.assertFalse((archive_dir / "empty_platform.zip").exists())

    def test_explicit_platforms(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _make_fake_defs(root)
            rc = build_def_arch.main([
                "--defs-dir", str(root),
                "--platforms", "impreza",
            ])
            self.assertEqual(rc, 0)
            self.assertTrue((root / "_archives" / "impreza.zip").is_file())
            self.assertFalse((root / "_archives" / "legacy.zip").exists())


class TestCheckCommand(unittest.TestCase):
    def test_missing_returns_nonzero(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _make_fake_defs(root)
            rc = build_def_arch.main(["--defs-dir", str(root), "--check"])
            self.assertEqual(rc, 2)

    def test_fresh_build_returns_zero(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _make_fake_defs(root)
            build_def_arch.main(["--defs-dir", str(root)])
            rc = build_def_arch.main(["--defs-dir", str(root), "--check"])
            self.assertEqual(rc, 0)


class TestCleanCommand(unittest.TestCase):
    def test_removes_stale_archives(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _make_fake_defs(root)
            build_def_arch.main(["--defs-dir", str(root)])
            # Add a stale archive referring to a non-existent platform.
            (root / "_archives" / "removed_platform.zip").write_bytes(b"PK")
            rc = build_def_arch.main(["--defs-dir", str(root), "--clean"])
            self.assertEqual(rc, 0)
            self.assertFalse((root / "_archives" / "removed_platform.zip").exists())
            # Legitimate archives should remain.
            self.assertTrue((root / "_archives" / "impreza.zip").is_file())


if __name__ == "__main__":
    unittest.main()
