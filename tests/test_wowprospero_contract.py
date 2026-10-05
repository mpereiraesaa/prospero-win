#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Check the WoW64 CPU backend against its two contracts.

Wine's wow64.dll resolves the i386 CPU backend's BTCpu* exports by name and
calls the ones it finds; the PE side of the backend reaches its Unix side by
index. Both are versioned data:

- every export named in `wowprospero.spec` has a definition in `cpu.c`, and
  the exports wow64 calls unconditionally are present;
- `enum pw_wow_funcs` in `wowprospero.h` and `__wine_unix_call_funcs[]` in
  `unix.c` list the same entries in the same order;
- the profiler and timing code that runs at every return from `run()` reads
  the TSC, never the system clock: on the PS5 a clock read is a system call,
  and run() returns hundreds of thousands of times a second.

When the pinned Wine checkout is available, the spec is also compared with the
`GET_PTR( BTCpu... )` list in `dlls/wow64/syscall.c`, so a new optional hook is
reported instead of silently ignored.
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / "wine/wowprospero"
SPEC = MODULE / "wowprospero.spec"
HEADER = MODULE / "wowprospero.h"
PE_SIDE = MODULE / "cpu.c"
UNIX_SIDE = MODULE / "unix.c"

# wow64 dereferences these without a NULL check (pinned syscall.c).
REQUIRED = {
    "BTCpuGetBopCode", "BTCpuGetContext", "BTCpuIsProcessorFeaturePresent",
    "BTCpuProcessInit", "BTCpuSetContext", "BTCpuSimulate",
    "__wine_get_unix_opcode",
}
EXPORT_RE = re.compile(r"^@\s+stdcall\s+(?:-\S+\s+)*([A-Za-z_][A-Za-z0-9_]*)\(", re.M)
ENUM_RE = re.compile(r"enum pw_wow_funcs\s*\{(.*?)\};", re.S)
TABLE_RE = re.compile(r"__wine_unix_call_funcs\[\]\s*=\s*\{(.*?)\};", re.S)
IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
# Called at every return from run(); a clock read belongs only in the
# once-per-period report they call.
PER_EXIT = ("profile_maybe_dump", "timing_leave")
CLOCK_READ_RE = re.compile(r"\b(?:clock_gettime|gettimeofday|timing_now_ns|profile_monotonic_ns)\s*\(")
GET_PTR_RE = re.compile(r"GET_PTR\(\s*((?:BTCpu|__wine_get_unix_opcode)[A-Za-z0-9_]*)\s*\)")


def fail(message: str) -> None:
    print(f"wowprospero contract: {message}", file=sys.stderr)
    sys.exit(1)


def wine_source() -> Path | None:
    candidates = []
    if os.environ.get("PROSPERO_WINE_SOURCE"):
        candidates.append(Path(os.environ["PROSPERO_WINE_SOURCE"]))
    candidates.append(ROOT / ".deps/wine/source")
    for candidate in candidates:
        if (candidate / "dlls/wow64/syscall.c").is_file():
            return candidate
    return None


def exports() -> list[str]:
    names = EXPORT_RE.findall(SPEC.read_text(encoding="utf-8"))
    if len(names) != len(set(names)):
        fail("duplicate export in wowprospero.spec")
    return names


def check_exports(names: list[str]) -> None:
    missing = REQUIRED - set(names)
    if missing:
        fail(f"spec lacks exports wow64 requires: {', '.join(sorted(missing))}")
    source = PE_SIDE.read_text(encoding="utf-8")
    for name in names:
        if not re.search(rf"\bWINAPI\s+{re.escape(name)}\s*\(", source):
            fail(f"export {name} has no WINAPI definition in cpu.c")


def check_unix_table() -> int:
    enum = ENUM_RE.search(HEADER.read_text(encoding="utf-8"))
    table = TABLE_RE.search(UNIX_SIDE.read_text(encoding="utf-8"))
    if not enum or not table:
        fail("unix-call enum or table not found")
    codes = IDENT_RE.findall(enum.group(1))
    if not codes or codes[-1] != "pw_wow_funcs_count":
        fail("enum pw_wow_funcs must end with pw_wow_funcs_count")
    codes = codes[:-1]
    handlers = IDENT_RE.findall(table.group(1))
    expected = [code.removeprefix("pw_wow_") for code in codes]
    if handlers != expected:
        fail(f"unix table order {handlers} does not match enum {expected}")
    return len(handlers)


def function_body(source: str, name: str) -> str:
    match = re.search(rf"^static [^;{{]*\b{name}\s*\([^)]*\)\s*\{{", source, re.M)
    if not match:
        fail(f"{name} not found in unix.c")
    depth, start = 1, match.end()
    for index in range(start, len(source)):
        depth += {"{": 1, "}": -1}.get(source[index], 0)
        if not depth:
            return source[start:index]
    fail(f"{name} has no closing brace")
    return ""


def check_per_exit_clock() -> None:
    source = UNIX_SIDE.read_text(encoding="utf-8")
    for name in PER_EXIT:
        read = CLOCK_READ_RE.search(function_body(source, name))
        if read:
            fail(f"{name} runs at every run() exit and must not call {read.group(0).rstrip('( ')}")


def check_pinned(names: list[str]) -> str:
    source = wine_source()
    if not source:
        return "skipped the pinned-source cross-check (no Wine checkout)"
    wanted = set(GET_PTR_RE.findall((source / "dlls/wow64/syscall.c").read_text(encoding="utf-8")))
    if not REQUIRED <= wanted:
        fail(f"pinned wow64 no longer resolves {', '.join(sorted(REQUIRED - wanted))}")
    unknown = set(names) - wanted
    if unknown:
        fail(f"exports not resolved by pinned wow64: {', '.join(sorted(unknown))}")
    optional = sorted(wanted - set(names))
    return f"pinned wow64 cross-check passed; optional hooks not exported: {', '.join(optional) or 'none'}"


def main() -> None:
    names = exports()
    check_exports(names)
    count = check_unix_table()
    check_per_exit_clock()
    note = check_pinned(names)
    print(f"wowprospero contract passed: {len(names)} exports, {count} unix calls; {note}")


if __name__ == "__main__":
    main()
