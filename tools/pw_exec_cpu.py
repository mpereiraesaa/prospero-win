#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Estimate the CPU time each thread spent in translated code, and compare builds.

A run with the execution clock on (the console trigger pw_wow_exec_timing,
or PW_WOW_EXEC_TIMING=1 on Linux) logs a cumulative line per translating
thread:

    wowprospero execution: tid=0024 cumulative=1 sample_cpu_ns=1508594000
        calls=32265545 samples=504504 stride=64 ... clock_errors=0

Only about one call into translated code in 64 is timed, so the thread's
total is estimated as sample_cpu_ns / samples * calls (here about 96.5 s).
The last line of each thread in a session is used. This works for any game;
it reads saved session logs (pw_gameplay_run.py --save) or ps5log captures.

Usage:
    pw_exec_cpu.py LOG...                      the busiest threads of each run
    pw_exec_cpu.py --baseline LOG... --candidate LOG...
                                               compare two builds, thread by thread

A comparison only counts a thread as faster or slower when every candidate
run is on the same side of every baseline run; anything in between is "no
clear difference". Alternate the builds' runs and repeat each at least twice.
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from statistics import median

EXECUTION = re.compile(r"wowprospero execution: (.*)")
# More failed clock reads than this share of samples makes a thread's
# estimate unreliable; one or two in hundreds of thousands don't matter.
CLOCK_ERROR_SHARE = 0.001


@dataclass
class Thread:
    tid: str
    seconds: float
    samples: int
    clock_errors: int

    @property
    def reliable(self) -> bool:
        return self.clock_errors <= self.samples * CLOCK_ERROR_SHARE


def estimates(text: str) -> dict[str, Thread]:
    """Each thread's estimated translated CPU time, from its last execution line."""
    last: dict[str, dict[str, str]] = {}
    for line in text.splitlines():
        match = EXECUTION.search(line)
        if not match:
            continue
        fields = dict(re.findall(r"(\w+)=(\w+)", match[1]))
        if "tid" in fields and fields.get("samples", "0").isdigit() and int(fields["samples"]) > 0:
            last[fields["tid"]] = fields
    return {tid: Thread(tid, int(f["sample_cpu_ns"]) / int(f["samples"]) * int(f["calls"]) / 1e9,
                        int(f["samples"]), int(f.get("clock_errors", "0")))
            for tid, f in last.items()}


def busiest(threads: dict[str, Thread], count: int) -> list[Thread]:
    return sorted(threads.values(), key=lambda thread: -thread.seconds)[:count]


def describe(thread: Thread) -> str:
    note = "" if thread.reliable else f" ({thread.clock_errors} clock errors: unreliable)"
    return f"tid={thread.tid} {thread.seconds:.1f} s{note}"


def compare(baseline: list[dict[str, Thread]], candidate: list[dict[str, Thread]],
            count: int) -> list[str]:
    """One line per thread that every run has, busiest in the baseline first."""
    runs = baseline + candidate
    common = set.intersection(*(set(run) for run in runs))
    order = sorted(common, key=lambda tid: -median(run[tid].seconds for run in baseline))[:count]
    if not order:
        return ["no thread appears in every run: nothing to compare"]
    lines = []
    for tid in order:
        before = [run[tid].seconds for run in baseline]
        after = [run[tid].seconds for run in candidate]
        if not all(run[tid].reliable for run in runs):
            verdict = "unreliable (too many clock errors in a run)"
        elif max(after) < min(before):
            verdict = "faster"
        elif min(after) > max(before):
            verdict = "slower"
        else:
            verdict = "no clear difference"
        change = (median(after) - median(before)) / median(before) * 100
        lines.append(f"tid={tid}: baseline {', '.join(f'{v:.1f}' for v in before)} s; "
                     f"candidate {', '.join(f'{v:.1f}' for v in after)} s; "
                     f"median {change:+.1f}%: {verdict}")
    return lines


def load(path: str) -> dict[str, Thread]:
    threads = estimates(Path(path).read_text(errors="replace"))
    if not threads:
        raise SystemExit(f"pw_exec_cpu: {path} has no execution lines "
                         "(was pw_wow_exec_timing on for the run?)")
    return threads


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("logs", nargs="*", help="saved session logs, one per run")
    parser.add_argument("--baseline", nargs="+", default=[], help="the reference build's runs")
    parser.add_argument("--candidate", nargs="+", default=[], help="the changed build's runs")
    parser.add_argument("--threads", type=int, default=3, help="how many threads to show (default 3)")
    args = parser.parse_args(argv)
    if bool(args.baseline) != bool(args.candidate):
        parser.error("--baseline and --candidate go together")
    if not args.logs and not args.baseline:
        parser.error("give saved session logs, or --baseline and --candidate")
    for path in args.logs:
        print(f"{path}: " + "; ".join(describe(t) for t in busiest(load(path), args.threads)))
    if args.baseline:
        if len(args.baseline) < 2 or len(args.candidate) < 2:
            print("note: with fewer than two runs per build, run-to-run variation can't be told apart from a change")
        for line in compare([load(p) for p in args.baseline], [load(p) for p in args.candidate], args.threads):
            print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
