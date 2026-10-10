#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Function names for the guest stack of PW_NATIVE_SLOW_SYSCALL lines.

The slow-system-service events (wow64 patch 0613, docs/native-system-
service-profile.md) name each stack entry as <basedllname>+<offset from the
module's load address>. This rewrites every entry whose module was given
with --module to <dll>!<function>, from addr2line over an unstripped build
of that DLL: the address it asks for is the DLL's preferred ImageBase, read
from its PE optional header, plus the offset. Entries of other modules, raw
addresses, unresolved ones and the ... of a cut line stay as they are, as
does every other line, so a whole log can go through. A release build keeps
its symbol table but no DWARF, so where addr2line answers ?? the nearest
preceding symbol from `nm -C -n` names the entry as <dll>!<symbol>+<delta>.
Spaces in a demangled name are dropped and commas become semicolons, so the
list still splits on commas and the line on whitespace.

Usage: symbolize_guest_stack.py (LOG | -) --module d3d9.dll=PATH [--module ...]
                                 [--addr2line i686-w64-mingw32-addr2line] [--nm i686-w64-mingw32-nm]
       symbolize_guest_stack.py --line TEXT --module d3d9.dll=PATH [...]
"""
from __future__ import annotations

import argparse
import re
import struct
import subprocess
import sys
from collections import defaultdict

ADDR2LINE = "i686-w64-mingw32-addr2line"
NM = "i686-w64-mingw32-nm"
STACK = re.compile(r"(PW_NATIVE_SLOW_SYSCALL version=1 .*\bstack=)(\S*)")
ENTRY = re.compile(r"^([^+!]+)\+([0-9a-f]+)$")


def image_base(path: str) -> int:
    """The preferred load address from the PE optional header (PE32 or PE32+)."""
    with open(path, "rb") as handle:
        head = handle.read(0x40)
        if len(head) < 0x40 or head[:2] != b"MZ":
            raise ValueError(f"{path}: not a PE image (no MZ header)")
        e_lfanew = struct.unpack_from("<I", head, 0x3c)[0]
        handle.seek(e_lfanew)
        nt = handle.read(4 + 20 + 32)
    if nt[:4] != b"PE\0\0":
        raise ValueError(f"{path}: not a PE image (no PE signature)")
    optional = 4 + 20
    magic = struct.unpack_from("<H", nt, optional)[0]
    if magic == 0x10b:
        return struct.unpack_from("<I", nt, optional + 28)[0]
    if magic == 0x20b:
        return struct.unpack_from("<Q", nt, optional + 24)[0]
    raise ValueError(f"{path}: unknown optional header magic {magic:#x}")


def run_addr2line(binary: str, path: str, addresses: list[int]) -> list[str | None]:
    """The function at each address, from `addr2line -f -C -e path`; None where it says ??."""
    if not addresses:
        return []
    run = subprocess.run([binary, "-f", "-C", "-e", path] + [f"{a:#x}" for a in addresses],
                         capture_output=True, text=True, check=True)
    lines = run.stdout.splitlines()
    names = []
    for index in range(len(addresses)):
        name = lines[2 * index].strip() if 2 * index < len(lines) else "??"
        names.append(None if name in ("??", "") else name)
    return names


def run_nm(binary: str, path: str) -> list[tuple[int, str]]:
    """The defined code symbols of path, (address, name), sorted by address."""
    run = subprocess.run([binary, "-C", "-n", "--defined-only", path],
                         capture_output=True, text=True, check=True)
    table = []
    for line in run.stdout.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and parts[1] in "tTwW" and parts[2]:
            table.append((int(parts[0], 16), parts[2]))
    return table


def nearest_symbol(table: list[tuple[int, str]], address: int) -> str | None:
    """symbol+delta for the last symbol at or before address, within 64 KiB."""
    import bisect
    index = bisect.bisect_right([entry[0] for entry in table], address) - 1
    if index < 0 or address - table[index][0] >= 0x10000:
        return None
    return f"{table[index][1]}+{address - table[index][0]:x}"


def parse_module(spec: str) -> tuple[str, str]:
    name, separator, path = spec.partition("=")
    if not separator or not name or not path:
        raise ValueError(f"--module takes dll=path, not {spec!r}")
    return name.lower(), path


def entries_of(line: str) -> list[str]:
    match = STACK.search(line)
    return match[2].split(",") if match else []


def wanted_offsets(lines: list[str], modules: dict) -> dict:
    """module -> sorted offsets the lines mention, for the modules given."""
    wanted: dict = defaultdict(set)
    for line in lines:
        for entry in entries_of(line):
            if (m := ENTRY.match(entry)) and m[1] in modules:
                wanted[m[1]].add(int(m[2], 16))
    return {module: sorted(offsets) for module, offsets in wanted.items()}


def clean(name: str) -> str:
    return name.replace(" ", "").replace(",", ";")


def resolve(wanted: dict, modules: dict, binary: str = ADDR2LINE, nm: str | None = NM) -> dict:
    """(module, offset) -> function: one addr2line run per module, then one
    nm run for the offsets addr2line left unresolved (nm=None skips that)."""
    table = {}
    for module, offsets in wanted.items():
        path, base = modules[module]
        names = run_addr2line(binary, path, [base + offset for offset in offsets])
        missing = []
        for offset, name in zip(offsets, names):
            if name:
                table[(module, offset)] = clean(name)
            else:
                missing.append(offset)
        if missing and nm:
            symbols = run_nm(nm, path)
            for offset in missing:
                if name := nearest_symbol(symbols, base + offset):
                    table[(module, offset)] = clean(name)
    return table


def rewrite(line: str, table: dict) -> str:
    def replace(match):
        entries = []
        for entry in match[2].split(","):
            m = ENTRY.match(entry)
            name = table.get((m[1], int(m[2], 16))) if m else None
            entries.append(f"{m[1]}!{name}" if name else entry)
        return match[1] + ",".join(entries)
    return STACK.sub(replace, line, count=1)


def symbolize(lines: list[str], modules: dict, binary: str = ADDR2LINE, nm: str | None = NM) -> list[str]:
    table = resolve(wanted_offsets(lines, modules), modules, binary, nm)
    return [rewrite(line, table) for line in lines]


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("log", nargs="?", help="the log to rewrite, or - for stdin")
    parser.add_argument("--line", help="one event line instead of a log")
    parser.add_argument("--module", action="append", default=[], metavar="DLL=PATH",
                        help="an unstripped build of a 32-bit module named in the events; repeatable")
    parser.add_argument("--addr2line", default=ADDR2LINE)
    parser.add_argument("--nm", default=NM, help="symbol-table fallback for release builds; '' disables it")
    args = parser.parse_args(argv)
    if (args.log is None) == (args.line is None):
        parser.error("give a log (or -) or --line, not both")
    try:
        modules = {}
        for spec in args.module:
            name, path = parse_module(spec)
            modules[name] = (path, image_base(path))
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 2
    if args.line is not None:
        lines = [args.line]
    elif args.log == "-":
        lines = sys.stdin.read().splitlines()
    else:
        with open(args.log, errors="replace") as handle:
            lines = handle.read().splitlines()
    try:
        out = symbolize(lines, modules, args.addr2line, args.nm or None)
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"addr2line or nm failed: {error}", file=sys.stderr)
        return 2
    sys.stdout.write("".join(line + "\n" for line in out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
