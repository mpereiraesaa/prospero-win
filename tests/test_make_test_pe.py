#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Cross-check the Python PE encoder against the C parser and mapper.

Two independent encoders exist on purpose: the C fixture header used by the
unit tests and `tools/make_test_pe.py`, which stages the on-disk samples the
hardware gate loads. This test feeds the Python output to the C
implementation through `inspect_pe`, so a defect in either encoder shows up
as a disagreement instead of certifying itself.
"""

from __future__ import annotations

import re
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INSPECT = ROOT / "build" / "host" / "inspect_pe"

sys.path.insert(0, str(ROOT / "tools"))

import make_test_pe  # noqa: E402


def run_inspect(*arguments: str) -> str:
    if not INSPECT.exists():
        raise unittest.SkipTest(f"{INSPECT} is not built")
    completed = subprocess.run([str(INSPECT), *arguments], check=False,
                               capture_output=True, text=True)
    if completed.returncode != 0:
        raise AssertionError(
            f"inspect_pe failed: {completed.returncode}\n"
            f"{completed.stdout}\n{completed.stderr}")
    return completed.stdout


def field(output: str, key: str) -> str:
    match = re.search(rf"\b{re.escape(key)}=(\S+)", output)
    if not match:
        raise AssertionError(f"{key} missing from:\n{output}")
    return match.group(1)


def graph_field(output: str, key: str) -> str:
    """Reads a field from the graph summary line specifically.

    `modules=` also appears in the import listing, so an unscoped search
    would silently read the wrong number.
    """
    for line in output.splitlines():
        if line.startswith("graph "):
            return field(line, key)
    raise AssertionError(f"no graph line in:\n{output}")


class SampleChainTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.path = Path(self.directory.name)
        self.addCleanup(self.directory.cleanup)

    def stage(self, pe32plus: bool = True) -> None:
        for name, data in make_test_pe.sample_chain(pe32plus=pe32plus).items():
            (self.path / name).write_bytes(data)

    def test_amd64_chain_maps_and_resolves(self) -> None:
        self.stage()
        output = run_inspect(str(self.path / "sample.exe"), "--dir",
                             str(self.path))

        self.assertEqual(field(output, "machine"), "amd64")
        self.assertEqual(field(output, "bits"), "64")
        self.assertEqual(field(output, "preferred_base"), "0x140000000")
        self.assertEqual(field(output, "relocatable"), "1")
        self.assertEqual(field(output, "dynamic_base"), "1")
        self.assertIn("native_execution=yes", output)

        # Three declared sections plus the generated .idata and .reloc.
        self.assertEqual(field(output, "sections"), "5")  # 3 declared + 2 generated
        for name in (".text", ".data", ".bss", ".idata", ".reloc"):
            self.assertIn(name, output)
        self.assertRegex(output, r"section \.text\s+rva=0x00001000.*r-x")
        self.assertRegex(output, r"section \.bss\s+.*raw=0\s+.*bss")

        # binkw32 is third-party and mapped; kernel32 is a host binding.
        self.assertRegex(output, r"binkw32\.dll\s+named=2\s+ordinal=0\s+local")
        self.assertRegex(output, r"KERNEL32\.dll\s+named=1\s+ordinal=1\s+host")

        # game -> binkw32 -> msvcrt, plus kernel32 from the root.
        self.assertEqual(graph_field(output, "modules"), "4")
        self.assertEqual(graph_field(output, "local"), "1")
        self.assertEqual(graph_field(output, "host"), "2")
        self.assertEqual(graph_field(output, "cycles"), "0")
        self.assertEqual(graph_field(output, "depth"), "2")

        # The root is loaded last: every dependency precedes it.
        order = re.findall(r"^  \d+ (\S+)\s+(\S+)", output, re.M)
        self.assertEqual(order[-1], ("root.exe", "root"))
        self.assertIn(("binkw32.dll", "local"), order)
        self.assertLess(order.index(("binkw32.dll", "local")),
                        order.index(("root.exe", "root")))

        # Every mapped module was rebased away from its preferred base.
        for line in output.splitlines():
            if " local " in line or " root " in line:
                self.assertRegex(line, r"relocs=[1-9]")
        self.assertEqual(field(output, "opens"), "2")
        self.assertEqual(field(output, "closes"), "2")

    def test_i386_chain_parses_but_is_not_natively_executable(self) -> None:
        self.stage(pe32plus=False)
        output = run_inspect(str(self.path / "sample.exe"), "--no-map")

        self.assertEqual(field(output, "machine"), "i386")
        self.assertEqual(field(output, "bits"), "32")
        self.assertEqual(field(output, "preferred_base"), "0x400000")
        # The console runs 64-bit user code only; see TECHNICAL_DETAILS.md.
        self.assertIn("native_execution=no", output)
        self.assertRegex(output, r"binkw32\.dll\s+named=2")
        self.assertEqual(field(output, "opens"), "1")
        self.assertEqual(field(output, "closes"), "1")
        self.assertIn("import KERNEL32.dll ordinal=291", output)
        self.assertIn("import KERNEL32.dll name=CreateFileA", output)

    def test_malformed_input_releases_root(self) -> None:
        path = self.path / "invalid.exe"
        path.write_bytes(b"invalid PE input")
        completed = subprocess.run([str(INSPECT), str(path), "--no-map"],
                                   capture_output=True, text=True)
        self.assertEqual(completed.returncode, 1, completed.stderr)
        self.assertIn("parse failed:", completed.stderr)
        self.assertIn("released opens=1 closes=1", completed.stdout)

    def test_missing_dependency_releases_root(self) -> None:
        self.stage()
        (self.path / "binkw32.dll").unlink()
        completed = subprocess.run([str(INSPECT), str(self.path / "sample.exe"),
                                    "--dir", str(self.path)],
                                   capture_output=True, text=True)
        self.assertEqual(completed.returncode, 1, completed.stderr)
        self.assertIn("load failed:", completed.stderr)
        self.assertEqual(field(completed.stdout, "opens"),
                         field(completed.stdout, "closes"))

    def test_encoder_output_is_deterministic(self) -> None:
        first = make_test_pe.sample_chain()
        second = make_test_pe.sample_chain()
        self.assertEqual(first, second)
        # A reproducible artifact is a precondition for hashed evidence.
        self.assertEqual(sorted(first), ["binkw32.dll", "sample.exe"])

    def test_application_diamond(self) -> None:
        """The tranche-2 fixture: a real application graph, pinned shape by shape.

        The EXE imports by name and by ordinal, one DLL forwards an export to
        the other, both DLLs have a DllMain and a TLS callback, and every one of
        those records a distinct value into the EXE's trace slots - so the
        fixture carries the ordering the loader must reproduce. The C parser
        accepts all three images.
        """
        images = make_test_pe.application_diamond()
        self.assertEqual(sorted(images), ["a.dll", "app.exe", "b.dll"])
        trace = make_test_pe.APP_IMAGE_BASE + make_test_pe.TRACE_RVA

        # Every module is PE32/i386 with a base the guest can address.
        for name, image in images.items():
            nt = struct.unpack_from("<I", image, 0x3C)[0]
            self.assertEqual(struct.unpack_from("<H", image, nt + 4)[0], 0x014C,
                             name)
            base = struct.unpack_from("<I", image, nt + 4 + 20 + 0x1C)[0]
            self.assertLess(base, 1 << 32, name)

        # The EXE's entry point, imports by name and by ordinal.
        app = images["app.exe"]
        nt = struct.unpack_from("<I", app, 0x3C)[0]
        optional = nt + 4 + 20
        entry = struct.unpack_from("<I", app, optional + 0x10)[0]
        text_rva = struct.unpack_from("<I", app, optional + struct.unpack_from(
            "<H", app, nt + 4 + 16)[0] + 12)[0]
        entry_offset = entry - text_rva + struct.unpack_from(
            "<I", app, optional + struct.unpack_from(
                "<H", app, nt + 4 + 16)[0] + 20)[0]
        self.assertEqual(bytes(app[entry_offset:entry_offset + 6]),
                         b"\xb8\x01\x00\x00\x00\xc3")
        self.assertIn(b"a.dll\0", app)
        self.assertIn(b"b.dll\0", app)

        # The callbacks record into the EXE's four trace dwords, in the order
        # the loader is expected to run them.
        def records(image: bytes, slots: tuple[int, ...],
                    values: tuple[int, ...]) -> None:
            for slot, value in zip(slots, values):
                self.assertIn(struct.pack("<BBII", 0xC7, 0x05, slot, value),
                              image)

        records(images["a.dll"], (trace, trace + 4), (0xA1, 0xA2))
        records(images["b.dll"], (trace + 8, trace + 12), (0xB1, 0xB2))
        self.assertIn(b"b.dll.Provided\0", images["a.dll"])
        for name, image in images.items():
            path = self.path / name
            path.write_bytes(image)
            self.assertEqual(field(run_inspect(str(path), "--no-map"),
                                  "machine"), "i386")

    def test_forwarder_export_and_tls(self) -> None:
        """The two fixture shapes tranche 2 needs, parsed back out of the bytes.

        A forwarded export is a function entry that points *inside* the export
        directory at an "other.dll.Target" string - that is what makes it a
        forwarder rather than code - and the TLS directory's callback array must
        hold the callback RVA followed by the terminating null entry. Both are
        read here the way a loader would read them, and the C parser then has to
        accept the same image.
        """
        spec = make_test_pe.Spec(
            name="fixture.dll", pe32plus=False, dll=True,
            sections=[make_test_pe.Section(".text", 0x60000020, b"\xc3\x90",
                                           virtual_size=0x1000),
                      make_test_pe.Section(".data", 0xc0000040, b"ABCD",
                                           virtual_size=0x1000)],
            exports=[make_test_pe.Export("Exported", rva=0x1000),
                     make_test_pe.Export("Forwarded",
                                         forwarder="other.dll.Target")],
            tls=make_test_pe.Tls(callbacks=(0x1000,), zero_fill=4,
                                 index_rva=0x2000))
        image = make_test_pe.build_pe(spec)

        nt = struct.unpack_from("<I", image, 0x3C)[0]
        optional = nt + 4 + 20
        optional_size = struct.unpack_from("<H", image, nt + 4 + 16)[0]
        directories = optional + optional_size - 8 * make_test_pe.DIRECTORY_ENTRIES
        sections = struct.unpack_from("<H", image, nt + 4 + 2)[0]
        table = optional + optional_size

        def rva_to_offset(rva: int) -> int:
            for index in range(sections):
                entry = table + index * 40
                virtual_size, virtual_address = struct.unpack_from(
                    "<II", image, entry + 8)
                raw_size, raw_offset = struct.unpack_from("<II", image, entry + 16)
                if virtual_address <= rva < virtual_address + max(virtual_size,
                                                                   raw_size):
                    return raw_offset + (rva - virtual_address)
            raise AssertionError(f"rva {rva:#x} is in no section")

        export_rva, export_size = struct.unpack_from("<II", image, directories)
        tls_rva, tls_size = struct.unpack_from(
            "<II", image, directories + 8 * make_test_pe.DIR_TLS)
        self.assertNotEqual(export_rva, 0)
        self.assertNotEqual(tls_rva, 0)

        first, second = struct.unpack_from("<II", image,
                                           rva_to_offset(export_rva + 40))
        self.assertEqual(first, 0x1000)
        self.assertTrue(export_rva <= second < export_rva + export_size,
                        "the forwarded entry must point inside the directory")
        forwarder = image[rva_to_offset(second):].split(b"\0", 1)[0]
        self.assertEqual(forwarder, b"other.dll.Target")

        index_va, callbacks_va = struct.unpack_from(
            "<II", image, rva_to_offset(tls_rva) + 8)
        # The directory holds virtual addresses - the image base plus the RVA
        # the caller supplied, 0x400000 being the PE32 default this spec leaves
        # in place - so converting back to a file offset means subtracting it.
        image_base = 0x400000
        self.assertEqual(index_va, image_base + 0x2000)
        callbacks = rva_to_offset(callbacks_va - image_base)
        self.assertEqual(struct.unpack_from("<I", image, callbacks)[0],
                         image_base + 0x1000)
        self.assertEqual(struct.unpack_from("<I", image, callbacks + 4)[0], 0)
        self.assertGreaterEqual(tls_size, 24)

        path = self.path / "fixture.dll"
        path.write_bytes(image)
        output = run_inspect(str(path), "--no-map")
        self.assertEqual(field(output, "machine"), "i386")

    def test_encoder_rejects_bad_specs(self) -> None:
        with self.assertRaises(ValueError):
            make_test_pe.build_pe(make_test_pe.Spec(name="empty.exe"))
        with self.assertRaises(ValueError):
            make_test_pe.build_pe(make_test_pe.Spec(
                name="misaligned.exe",
                image_base=0x140000800,
                sections=[make_test_pe.Section(
                    ".text", make_test_pe.SCN_MEM_READ, b"\xc3")],
            ))


if __name__ == "__main__":
    unittest.main()
