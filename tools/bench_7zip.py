#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Read 7-Zip benchmark (`7za b`) output: the compression and decompression
speed (KiB/s) and rating (MIPS) of the Avr: row, and the Tot: rating.

usage: bench_7zip.py [--label L] [--median] [--json] LOG...
One line per log, or with --median one line of per-field medians.
"""
from __future__ import annotations

import argparse
import json
import statistics
import sys
from dataclasses import asdict, dataclass
from pathlib import Path


@dataclass
class BenchResult:
    compress_kib_s: int
    compress_mips: int
    decompress_kib_s: int
    decompress_mips: int
    total_mips: int


def parse(text: str) -> BenchResult:
    """The Avr: and Tot: rows of one benchmark run.

    Avr: <speed> <usage> <R/U> <rating> | <speed> <usage> <R/U> <rating>
    Tot:         <usage> <R/U> <rating>
    """
    average = total = None
    for line in text.splitlines():
        words = line.replace("|", " | ").split()
        if not words:
            continue
        if words[0] == "Avr:":
            average = words
        elif words[0] == "Tot:":
            total = words
    if average is None or total is None:
        raise ValueError("no Avr:/Tot: rows: the benchmark did not finish")
    try:
        bar = average.index("|")
        compress, decompress = average[1:bar], average[bar + 1:]
        if len(compress) != 4 or len(decompress) != 4 or len(total) != 4:
            raise ValueError("unexpected benchmark row layout")
        return BenchResult(int(compress[0]), int(compress[3]), int(decompress[0]),
                           int(decompress[3]), int(total[3]))
    except (ValueError, IndexError) as error:
        raise ValueError(f"malformed benchmark row: {error}") from None


def median(results: list[BenchResult]) -> BenchResult:
    if not results:
        raise ValueError("no results")
    fields = asdict(results[0]).keys()
    return BenchResult(**{name: int(statistics.median(getattr(r, name) for r in results))
                          for name in fields})


def describe(label: str, result: BenchResult) -> str:
    return (f"{label}: compress {result.compress_mips} MIPS ({result.compress_kib_s} KiB/s)"
            f"  decompress {result.decompress_mips} MIPS ({result.decompress_kib_s} KiB/s)"
            f"  total {result.total_mips} MIPS")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--label", default="")
    parser.add_argument("--median", action="store_true")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("logs", nargs="+", type=Path)
    options = parser.parse_args(argv)
    try:
        results = [parse(log.read_text(errors="replace")) for log in options.logs]
    except (OSError, ValueError) as error:
        print(f"bench_7zip: {error}", file=sys.stderr)
        return 1
    shown = [median(results)] if options.median else results
    for i, result in enumerate(shown):
        label = options.label or ("median" if options.median else str(options.logs[i]))
        print(json.dumps({"label": label, **asdict(result)}) if options.json
              else describe(label, result))
    return 0


if __name__ == "__main__":
    sys.exit(main())
