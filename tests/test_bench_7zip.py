#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tools/bench_7zip.py reads 7-Zip's benchmark output exactly."""
from __future__ import annotations

import io
import sys
import tempfile
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import bench_7zip  # noqa: E402

# The tail of `7za b -mmt1 -md22` from 7-Zip 25.01 (x86) under Wine WoW64.
SAMPLE = """
7-Zip (a) 25.01 (x86) : Copyright (c) 1999-2025 Igor Pavlov : 2025-08-03

 mt1 d22
1T CPU Freq (MHz):  3750  3549  3723  3727  3721  3726  3755

RAM size:    4095 MB,  # CPU hardware threads:   1 / 20 : 1 / FFFFF
RAM usage:     57 MB,  # Benchmark threads:      1

                       Compressing  |                  Decompressing
Dict     Speed Usage    R/U Rating  |      Speed Usage    R/U Rating
         KiB/s     %   MIPS   MIPS  |      KiB/s     %   MIPS   MIPS

22:       5614   100   5462   5462  |      50481   100   4310   4310
----------------------------------  | ------------------------------
Avr:      5614   100   5462   5462  |      50481   100   4310   4310
Tot:             100   4886   4886
"""


def expect_error(text: str, fragment: str) -> None:
    try:
        bench_7zip.parse(text)
    except ValueError as error:
        assert fragment in str(error), error
    else:
        raise AssertionError(f"accepted: {text!r}")


def main() -> None:
    result = bench_7zip.parse(SAMPLE)
    assert result == bench_7zip.BenchResult(5614, 5462, 50481, 4310, 4886), result

    # An interrupted run (no Avr:/Tot:) and a changed layout are refused.
    expect_error(SAMPLE.split("Avr:")[0], "did not finish")
    expect_error(SAMPLE.replace("Tot:             100", "Tot:"), "layout")
    expect_error(SAMPLE.replace("5462  |", "x  |"), "malformed")
    expect_error("", "did not finish")

    # Medians are per field; an odd count picks the middle run.
    runs = [bench_7zip.BenchResult(1, 10, 100, 1000, 7),
            bench_7zip.BenchResult(3, 30, 300, 3000, 5),
            bench_7zip.BenchResult(2, 20, 200, 2000, 9)]
    assert bench_7zip.median(runs) == bench_7zip.BenchResult(2, 20, 200, 2000, 7)
    try:
        bench_7zip.median([])
    except ValueError:
        pass
    else:
        raise AssertionError("median of nothing")

    with tempfile.TemporaryDirectory() as scratch:
        good = Path(scratch) / "a.log"
        good.write_text(SAMPLE)
        bad = Path(scratch) / "b.log"
        bad.write_text("wine: Unhandled illegal instruction\n")
        out = io.StringIO()
        with redirect_stdout(out):
            assert bench_7zip.main(["--label", "x", good.as_posix()]) == 0
            assert bench_7zip.main(["--json", "--median", good.as_posix(), good.as_posix()]) == 0
        lines = out.getvalue().splitlines()
        assert lines[0] == ("x: compress 5462 MIPS (5614 KiB/s)  decompress 4310 MIPS "
                            "(50481 KiB/s)  total 4886 MIPS"), lines[0]
        assert lines[1] == ('{"label": "median", "compress_kib_s": 5614, "compress_mips": 5462, '
                            '"decompress_kib_s": 50481, "decompress_mips": 4310, '
                            '"total_mips": 4886}'), lines[1]
        err = io.StringIO()
        with redirect_stderr(err):
            assert bench_7zip.main([bad.as_posix()]) == 1
            assert bench_7zip.main([(Path(scratch) / "missing.log").as_posix()]) == 1
        assert "did not finish" in err.getvalue()
    print("bench 7zip passed: Avr/Tot rows, refusals, medians, text and JSON output")


if __name__ == "__main__":
    main()
