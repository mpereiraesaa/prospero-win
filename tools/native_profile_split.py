#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Where each native WoW64 thread's time went, from PW_NATIVE_PROFILE=1 logs.

wow64native reports cumulative per-thread counters (version 2, at least every
2 s of a thread's TSC time): the TSC, the time spent on the host side between
a guest-to-host and the next host-to-guest switch, by what entered the host
(Unix calls, syscalls, anything else), the FS-switch sysarch ticks and the
entry counts. This takes the first and last report of each thread inside the
chosen window and prints, per thread, the share of wall time spent

  unix%     on the host in Unix calls (Vulkan batch replay, the driver,
            non-batched Vulkan calls, other Unix libraries)
  syscall%  on the host in syscalls (including waiting in them)
  other%    on the host for anything else (exceptions, callbacks)
  fs%       in the FS-switch sysarch calls themselves
  guest%    the rest: the game's and its DLLs' own code (DXVK, d3d9),
            plus any time the thread was descheduled while there

with Unix calls and syscalls per second. A Unix call that calls back into
the guest splits: the part after the callback counts as a syscall. The
busiest Vulkan thread (DXVK's CS thread) is the one with the highest Unix-call
rate.

Usage: native_profile_split.py LOG [--from SECONDS] [--to SECONDS] [--min-wall SECONDS]
    --from/--to bound the window in seconds after the first report in the log.
"""
from __future__ import annotations

import argparse
import re
import sys

LINE = re.compile(r"PW_NATIVE_PROFILE version=2 (.*)$")
FIELD = re.compile(r"(\w+)=([0-9a-fA-F]+)")
HEX = {"tid", "teb"}


def parse(lines):
    """[(tid, fields)] in log order, for version-2 reports with a TSC rate."""
    reports = []
    for line in lines:
        match = LINE.search(line)
        if not match:
            continue
        fields = {key: int(value, 16 if key in HEX else 10) for key, value in FIELD.findall(match[1])}
        if fields.get("tsc_hz") and "tsc" in fields:
            reports.append(fields)
    return reports


def split(reports, start=None, end=None, min_wall=0.5):
    """Per-thread rows over the window; seconds are relative to the first report."""
    if not reports:
        return []
    hz = reports[0]["tsc_hz"]
    origin = min(r["tsc"] for r in reports)
    threads: dict[int, list[dict]] = {}
    for r in reports:
        seconds = (r["tsc"] - origin) / hz
        if (start is not None and seconds < start) or (end is not None and seconds > end):
            continue
        threads.setdefault(r["tid"], []).append(r)
    rows = []
    for tid, items in threads.items():
        first, last = items[0], items[-1]
        wall = last["tsc"] - first["tsc"]
        if wall <= 0 or wall / hz < min_wall:
            continue
        delta = {key: last[key] - first[key] for key in last if key in first and key not in HEX}
        unix, syscall, other = delta["unix_host_ticks"], delta["syscall_host_ticks"], delta["other_host_ticks"]
        fs = delta["host_sysarch_ticks"] + delta["guest_sysarch_ticks"]
        guest = max(0, wall - unix - syscall - other - fs)
        rows.append(dict(tid=tid, seconds=wall / hz, unix=100 * unix / wall, syscall=100 * syscall / wall,
                         other=100 * other / wall, fs=100 * fs / wall, guest=100 * guest / wall,
                         unix_rate=delta["unix_calls"] * hz / wall,
                         syscall_rate=delta["syscall_calls"] * hz / wall))
    rows.sort(key=lambda row: -row["unix_rate"])
    return rows


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("log")
    parser.add_argument("--from", dest="start", type=float)
    parser.add_argument("--to", dest="end", type=float)
    parser.add_argument("--min-wall", type=float, default=0.5)
    args = parser.parse_args(argv)
    with open(args.log, errors="replace") as handle:
        reports = parse(handle)
    rows = split(reports, args.start, args.end, args.min_wall)
    if not rows:
        print("no PW_NATIVE_PROFILE version=2 reports in the window", file=sys.stderr)
        return 1
    print(f"{'tid':>6} {'seconds':>8} {'unix%':>6} {'syscall%':>8} {'other%':>6} {'fs%':>5} "
          f"{'guest%':>6} {'unix/s':>9} {'sys/s':>9}")
    for r in rows:
        print(f"{r['tid']:>6x} {r['seconds']:>8.1f} {r['unix']:>6.1f} {r['syscall']:>8.1f} {r['other']:>6.1f} "
              f"{r['fs']:>5.1f} {r['guest']:>6.1f} {r['unix_rate']:>9.0f} {r['syscall_rate']:>9.0f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
