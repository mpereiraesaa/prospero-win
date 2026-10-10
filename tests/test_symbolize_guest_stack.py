#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tools/symbolize_guest_stack.py: the ImageBase from PE32 and PE32+ headers,
the addresses it asks addr2line for (ImageBase plus offset, once per module),
the entries it rewrites and the ones it leaves alone, the name cleaning, the
addr2line output parsing against a stand-in, and the command line."""

import os
import stat
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import symbolize_guest_stack as sgs  # noqa: E402

EVENT = ("12\t34\tINFO\tWINE PW_NATIVE_SLOW_SYSCALL version=1 tid=004c tsc=150000000 ticks=30000000 code=0004 "
         "name=NtWaitForSingleObject arg0=00000120 arg1=00000000 arg2=00000000 status=00000000 "
         "stack=ntdll.dll+1234,d3d9.dll+1a2b3c,d3d9.dll+2000,gtaiv.exe+3000,d3d9.dll+1a2b3c,7bc00000,d3d9.dll+9,...")


def pe_header(magic: int, base: int) -> bytes:
    """A DOS header, a PE signature, a COFF header and the start of an optional header."""
    dos = bytearray(0x40)
    dos[:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3c, 0x40)
    optional = bytearray(0x70)
    struct.pack_into("<H", optional, 0, magic)
    if magic == 0x10b:
        struct.pack_into("<I", optional, 28, base)
    else:
        struct.pack_into("<Q", optional, 24, base)
    return bytes(dos) + b"PE\0\0" + bytes(20) + bytes(optional)


def main() -> int:
    with tempfile.TemporaryDirectory() as directory:
        base = Path(directory)
        (base / "d3d9.dll").write_bytes(pe_header(0x10b, 0x10000000))
        (base / "wide.dll").write_bytes(pe_header(0x20b, 0x180000000))
        (base / "odd.dll").write_bytes(pe_header(0x107, 0))
        (base / "text.txt").write_bytes(b"not a PE image")
        assert sgs.image_base(str(base / "d3d9.dll")) == 0x10000000
        assert sgs.image_base(str(base / "wide.dll")) == 0x180000000
        for bad in ("odd.dll", "text.txt"):
            try:
                sgs.image_base(str(base / bad))
            except ValueError as error:
                assert bad in str(error)
            else:
                raise AssertionError(f"{bad} accepted")
        assert sgs.parse_module("D3D9.dll=/x/y") == ("d3d9.dll", "/x/y")
        for bad in ("d3d9.dll", "=/x", "d3d9.dll="):
            try:
                sgs.parse_module(bad)
            except ValueError:
                pass
            else:
                raise AssertionError(f"{bad} accepted")

        # Offsets are collected per given module, once each, and asked for at ImageBase + offset.
        modules = {"d3d9.dll": (str(base / "d3d9.dll"), 0x10000000)}
        lines = [EVENT, "other line", "PW_NATIVE_SLOW_SYSCALL version=1 tid=1 stack=d3d9.dll+40,ntdll.dll+8"]
        assert sgs.wanted_offsets(lines, modules) == {"d3d9.dll": [0x9, 0x40, 0x2000, 0x1a2b3c]}
        assert sgs.entries_of("no stack here") == []
        calls = []

        def fake_addr2line(binary, path, addresses):
            calls.append((binary, path, addresses))
            table = {0x101a2b3c: "dxvk::DxvkCsThread::synchronize(unsigned long, unsigned int)",
                     0x10002000: "D3D9DeviceEx::Flush<true, 3>", 0x10000040: "??"}
            return [None if table.get(a, "??") == "??" else table[a] for a in addresses]

        real = sgs.run_addr2line
        sgs.run_addr2line = fake_addr2line
        try:
            out = sgs.symbolize(lines, modules, "fake-addr2line", None)
        finally:
            sgs.run_addr2line = real
        assert calls == [("fake-addr2line", str(base / "d3d9.dll"), [0x10000009, 0x10000040, 0x10002000, 0x101a2b3c])], calls
        assert out[0].endswith(
            "stack=ntdll.dll+1234,d3d9.dll!dxvk::DxvkCsThread::synchronize(unsignedlong;unsignedint),"
            "d3d9.dll!D3D9DeviceEx::Flush<true;3>,gtaiv.exe+3000,"
            "d3d9.dll!dxvk::DxvkCsThread::synchronize(unsignedlong;unsignedint),7bc00000,d3d9.dll+9,..."), out[0]
        assert out[0].startswith(EVENT.split("stack=")[0])
        assert out[1] == "other line" and out[2].endswith("stack=d3d9.dll+40,ntdll.dll+8")
        assert sgs.symbolize([EVENT], {}, "unused", None) == [EVENT]      # no modules: nothing asked, nothing changed
        assert sgs.resolve({}, modules, "unused", None) == {}

        # run_addr2line parses function/location pairs from a stand-in addr2line.
        script = base / "addr2line"
        script.write_text("#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$(dirname \"$0\")/args\"\n"
                          "shift 4\nfor a in \"$@\"; do if [ \"$a\" = 0x10000009 ]; then printf '??\\n??:0\\n'; "
                          "else printf 'fn_%s\\nfile.cpp:12\\n' \"$a\"; fi; done\n")
        script.chmod(script.stat().st_mode | stat.S_IEXEC)
        names = sgs.run_addr2line(str(script), str(base / "d3d9.dll"), [0x10000009, 0x101a2b3c])
        assert names == [None, "fn_0x101a2b3c"], names
        assert (base / "args").read_text().split() == ["-f", "-C", "-e", str(base / "d3d9.dll"), "0x10000009", "0x101a2b3c"]
        assert sgs.run_addr2line(str(script), str(base / "d3d9.dll"), []) == []

        # A release build has no DWARF: addr2line says ?? and the nearest
        # preceding nm symbol (within 64 KiB) names the entry as symbol+delta.
        nm = base / "nm"
        nm.write_text("#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$(dirname \"$0\")/nm-args\"\n"
                      "printf '10000000 T start\\n10000030 t dxvk::DxvkContext::flushCommandList(VkDebugUtilsLabelEXT const*)\\n"
                      "10001000 r rodata_not_code\\n10002000 T D3D9DeviceEx::Flush<true, 3>\\n10200000 W late\\n'\n")
        nm.chmod(nm.stat().st_mode | stat.S_IEXEC)
        symbols = sgs.run_nm(str(nm), str(base / "d3d9.dll"))
        assert symbols == [(0x10000000, "start"), (0x10000030, "dxvk::DxvkContext::flushCommandList(VkDebugUtilsLabelEXT const*)"),
                           (0x10002000, "D3D9DeviceEx::Flush<true, 3>"), (0x10200000, "late")], symbols
        assert (base / "nm-args").read_text().split() == ["-C", "-n", "--defined-only", str(base / "d3d9.dll")]
        assert sgs.nearest_symbol(symbols, 0x10000040) == "dxvk::DxvkContext::flushCommandList(VkDebugUtilsLabelEXT const*)+10"
        assert sgs.nearest_symbol(symbols, 0x10002000) == "D3D9DeviceEx::Flush<true, 3>+0"
        assert sgs.nearest_symbol(symbols, 0x10012000) is None      # 64 KiB past Flush: not it
        assert sgs.nearest_symbol(symbols, 0x0fffffff) is None      # before the first symbol
        sgs.run_addr2line = fake_addr2line
        try:
            out = sgs.symbolize(lines, modules, "fake-addr2line", str(nm))
        finally:
            sgs.run_addr2line = real
        assert "d3d9.dll!dxvk::DxvkContext::flushCommandList(VkDebugUtilsLabelEXTconst*)+10," in out[2], out[2]
        assert out[0].endswith(",d3d9.dll!start+9,..."), out[0]          # addr2line's ?? at +9, nm's start+9

        # The command line: a log file, stdin, --line, and the refusals.
        log = base / "session.log"
        log.write_text("\n".join(lines) + "\n")
        tool = [sys.executable, str(ROOT / "tools/symbolize_guest_stack.py")]
        run = subprocess.run(tool + [str(log), "--module", f"d3d9.dll={base / 'd3d9.dll'}", "--addr2line", str(script), "--nm", ""],
                             capture_output=True, text=True)
        assert run.returncode == 0, run.stderr
        rows = run.stdout.splitlines()
        assert len(rows) == 3 and "d3d9.dll!fn_0x101a2b3c,d3d9.dll!fn_0x10002000,gtaiv.exe+3000" in rows[0], rows
        assert ",d3d9.dll+9,..." in rows[0] and rows[2].endswith("stack=d3d9.dll!fn_0x10000040,ntdll.dll+8")
        run = subprocess.run(tool + ["-", "--module", f"d3d9.dll={base / 'd3d9.dll'}", "--addr2line", str(script), "--nm", ""],
                             input=EVENT + "\n", capture_output=True, text=True)
        assert run.returncode == 0 and "d3d9.dll!fn_0x101a2b3c" in run.stdout
        run = subprocess.run(tool + ["--line", EVENT, "--module", f"D3D9.DLL={base / 'd3d9.dll'}", "--addr2line", str(script), "--nm", ""],
                             capture_output=True, text=True)
        assert run.returncode == 0 and run.stdout.count("\n") == 1 and "d3d9.dll!fn_0x101a2b3c" in run.stdout
        run = subprocess.run(tool + ["--line", EVENT, "--module", f"d3d9.dll={base / 'text.txt'}"], capture_output=True, text=True)
        assert run.returncode == 2 and "not a PE image" in run.stderr
        run = subprocess.run(tool + ["--line", EVENT, "--module", "d3d9.dll=" + str(base / "missing.dll")],
                             capture_output=True, text=True)
        assert run.returncode == 2 and "missing.dll" in run.stderr
        run = subprocess.run(tool + ["--line", EVENT, "--module", f"d3d9.dll={base / 'd3d9.dll'}", "--addr2line",
                                     str(base / "no-such-addr2line")], capture_output=True, text=True)
        assert run.returncode == 2 and "addr2line or nm failed" in run.stderr
        run = subprocess.run(tool + [str(log), "--line", EVENT], capture_output=True, text=True)
        assert run.returncode == 2 and "not both" in run.stderr
        run = subprocess.run(tool + [str(log)], capture_output=True, text=True)
        assert run.returncode == 0 and run.stdout == "\n".join(lines) + "\n"     # no module: a copy
    print("symbolize_guest_stack: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
