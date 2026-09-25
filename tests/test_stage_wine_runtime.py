#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Contract tests for packaging the exact validated Wine runtime files."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import make_test_pe  # noqa: E402

VALIDATOR_SPEC = importlib.util.spec_from_file_location(
    "wine_runtime_validator", ROOT / "tools/validate_wine_runtime.py")
assert VALIDATOR_SPEC and VALIDATOR_SPEC.loader
VALIDATOR = importlib.util.module_from_spec(VALIDATOR_SPEC)
VALIDATOR_SPEC.loader.exec_module(VALIDATOR)


def pe32_dll(name: str) -> bytes:
    return make_test_pe.build_pe(make_test_pe.Spec(
        name=name,
        pe32plus=False,
        dll=True,
        sections=[make_test_pe.Section(
            ".text",
            make_test_pe.SCN_CNT_CODE | make_test_pe.SCN_MEM_READ |
            make_test_pe.SCN_MEM_EXECUTE,
            bytes([0x31, 0xC0, 0xC3]),
        )],
    ))


class StageWineRuntime(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = Path(tempfile.mkdtemp(prefix="wine-stage-"))
        self.source = self.temporary / "distribution"
        self.module = self.source / "lib/i386-windows/ntdll.dll"
        self.module.parent.mkdir(parents=True)
        self.module.write_bytes(pe32_dll("ntdll.dll"))
        nls = self.source / "nls/locale.nls"
        nls.parent.mkdir(parents=True)
        nls.write_bytes(b"synthetic nls data\0")
        self.wine_source = self.temporary / "wine-source"
        self.wine_source.mkdir()
        (self.wine_source / "VERSION").write_text(
            "Wine version 11.17\n", encoding="utf-8")
        subprocess.run(["git", "init", "--quiet", str(self.wine_source)],
                       check=True)
        subprocess.run(["git", "-C", str(self.wine_source), "add", "VERSION"],
                       check=True)
        subprocess.run([
            "git", "-C", str(self.wine_source),
            "-c", "user.email=tests@prospero-win.invalid",
            "-c", "user.name=prospero-win tests",
            "-c", "commit.gpgsign=false", "commit", "--quiet", "-m", "pin",
        ], check=True)
        self.commit = subprocess.run(
            ["git", "-C", str(self.wine_source), "rev-parse", "HEAD"],
            check=True, capture_output=True, text=True).stdout.strip()
        self.manifest = self.source / "wine-runtime-manifest.json"
        with contextlib.redirect_stdout(io.StringIO()):
            VALIDATOR.main([
                "write", "--distribution", str(self.source),
                "--wine-source", str(self.wine_source), "--wine-commit",
                self.commit, "--library", "lib/i386-windows", "--data",
                "nls/locale.nls", "--out", str(self.manifest),
            ])

    def tearDown(self) -> None:
        shutil.rmtree(self.temporary)

    def run_stage(self, destination: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run([
            sys.executable, str(ROOT / "tools/stage_wine_runtime.py"),
            "--source", str(self.source), "--destination", str(destination),
            "--expect-commit", self.commit,
        ], check=False, capture_output=True, text=True)

    def test_copies_only_manifest_files_and_manifest(self) -> None:
        destination = self.temporary / "package/runtime"
        completed = self.run_stage(destination)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual((destination / "lib/i386-windows/ntdll.dll").read_bytes(),
                         self.module.read_bytes())
        self.assertEqual((destination / "nls/locale.nls").read_bytes(),
                         b"synthetic nls data\0")
        self.assertTrue((destination / "wine-runtime-manifest.json").is_file())

    def test_digest_mismatch_leaves_no_partial_package(self) -> None:
        destination = self.temporary / "bad-package/runtime"
        self.module.write_bytes(self.module.read_bytes() + b"tamper")
        completed = self.run_stage(destination)
        self.assertNotEqual(completed.returncode, 0)
        self.assertFalse(destination.exists())

    def test_refuses_symlinked_runtime_module(self) -> None:
        real = self.temporary / "real-ntdll.dll"
        real.write_bytes(self.module.read_bytes())
        self.module.unlink()
        self.module.symlink_to(real)
        destination = self.temporary / "linked-package/runtime"
        completed = self.run_stage(destination)
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("symlink", completed.stderr)
        self.assertFalse(destination.exists())


if __name__ == "__main__":
    unittest.main()
