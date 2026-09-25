#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Test recursive pinned-Wine PE module closure planning."""

import tempfile
import unittest
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from wine_runtime_modules import resolve_closure  # noqa: E402


class RuntimeModuleClosureTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.source = Path(self.directory.name)

    def tearDown(self):
        self.directory.cleanup()

    def add_module(self, name, imports=""):
        path = self.source / "dlls" / name
        path.mkdir(parents=True)
        (path / "Makefile.in").write_text(f"IMPORTS = {imports}\n", encoding="ascii")

    def add_library(self, name):
        path = self.source / "libs" / name
        path.mkdir(parents=True)
        (path / "Makefile.in").write_text("", encoding="ascii")

    def test_resolves_application_roots_and_recursive_pe_dependencies(self):
        self.add_module("appui", "user32 uuid $(PNG_PE_LIBS)")
        self.add_module("user32", "gdi32 kernelbase")
        self.add_module("gdi32", "user32 ntdll")
        self.add_module("kernelbase", "ntdll uuid")
        self.add_module("kernel32", "kernelbase ntdll")
        self.add_module("ntdll")
        self.add_library("uuid")

        closure = resolve_closure(self.source, ["APPUI.DLL"])

        self.assertEqual(closure["modules"], [
            "appui", "gdi32", "kernel32", "kernelbase", "ntdll", "user32",
        ])
        self.assertEqual(closure["library_imports"], ["uuid"])
        self.assertEqual(closure["dependencies"]["user32"], ["gdi32", "kernelbase"])

    def test_missing_dependency_is_not_silently_omitted(self):
        self.add_module("appui", "missingdll")
        with self.assertRaisesRegex(ValueError, "unknown Wine dependency missingdll"):
            resolve_closure(self.source, ["appui"])

    def test_rejects_unsafe_module_names(self):
        with self.assertRaisesRegex(ValueError, "invalid Wine module name"):
            resolve_closure(self.source, ["../ntdll"])


if __name__ == "__main__":
    unittest.main()
