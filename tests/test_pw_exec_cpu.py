#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""pw_exec_cpu.py: per-thread translated CPU estimates and build comparisons."""
from __future__ import annotations

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True  # no tools/__pycache__ for the publication audit
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import pw_exec_cpu as cpu  # noqa: E402


def row(tid: str, cpu_ns: int, calls: int, samples: int, errors: int = 0) -> str:
    return (f"REC seq=1 t=1.0 WINESERVER wowprospero execution: tid={tid} cumulative=1 "
            f"sample_cpu_ns={cpu_ns} calls={calls} samples={samples} stride=64 "
            f"clock_batch_read_ns=852 clock_resolution_ns=1000 clock_errors={errors}")


def session(main: float, worker: float, errors: int = 0) -> str:
    """A run whose main thread and worker spent these seconds in translated code:
    1000 samples of main/1000 s each, scaled by 64 calls per sample."""
    return "\n".join([
        "PW_REPORT/1 build=abc profile=game cycle=0 pid=7 time=1790000000",
        row("0024", 1, 64, 1),  # an earlier cumulative line, replaced by the last
        row("0024", int(main / 64 * 1e9), 64_000, 1000, errors),
        row("00b0", int(worker / 64 * 1e9), 64_000, 1000),
        row("0030", 0, 0, 0),  # a thread that was never sampled
        "REC seq=9 t=9.0 WINESERVER wowprospero timing: tid=0024 run=40.0%",
    ]) + "\n"


def run(*args: str) -> str:
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        assert cpu.main(list(args)) == 0
    return out.getvalue()


class Estimates(unittest.TestCase):
    def test_last_line_per_thread(self):
        threads = cpu.estimates(session(96.5, 71.3))
        self.assertEqual(sorted(threads), ["0024", "00b0"])
        self.assertAlmostEqual(threads["0024"].seconds, 96.5, places=6)
        self.assertAlmostEqual(threads["00b0"].seconds, 71.3, places=6)
        self.assertEqual(threads["0024"].samples, 1000)

    def test_console_example(self):
        # The line from docs/DEBUGGING_GUIDE.md: 1.508594 s / 504,504 * 32,265,545.
        threads = cpu.estimates(row("0024", 1508594000, 32265545, 504504))
        self.assertAlmostEqual(threads["0024"].seconds, 96.48, places=2)

    def test_no_lines(self):
        self.assertEqual(cpu.estimates("REC seq=1 t=1 PW_WINE64 session_end reason=wine-exit\n"), {})

    def test_clock_errors(self):
        self.assertTrue(cpu.estimates(session(90, 70, errors=1))["0024"].reliable)
        unreliable = cpu.estimates(session(90, 70, errors=2))["0024"]
        self.assertFalse(unreliable.reliable)
        self.assertEqual(cpu.describe(unreliable), "tid=0024 90.0 s (2 clock errors: unreliable)")

    def test_busiest(self):
        threads = cpu.estimates(session(50, 70))
        self.assertEqual([t.tid for t in cpu.busiest(threads, 1)], ["00b0"])


class Compare(unittest.TestCase):
    def verdicts(self, baseline, candidate):
        before = [cpu.estimates(session(*runs)) for runs in baseline]
        after = [cpu.estimates(session(*runs)) for runs in candidate]
        return cpu.compare(before, after, 3)

    def test_faster_slower_and_overlap(self):
        lines = self.verdicts([(97.0, 71.5), (97.9, 71.3)], [(94.0, 72.0), (95.9, 71.4)])
        self.assertEqual(lines[0], "tid=0024: baseline 97.0, 97.9 s; candidate 94.0, 95.9 s; median -2.6%: faster")
        self.assertTrue(lines[1].endswith("no clear difference"), lines[1])
        lines = self.verdicts([(90.0, 70.0), (90.5, 70.0)], [(91.0, 70.2), (92.0, 70.0)])
        self.assertTrue(lines[0].endswith("slower"), lines[0])

    def test_equal_edges_are_not_a_gain(self):
        # A candidate run equal to the best baseline run is inside the spread.
        lines = self.verdicts([(97.0, 70), (98.0, 70)], [(97.0, 70), (96.0, 70)])
        self.assertTrue(lines[0].endswith("no clear difference"), lines[0])

    def test_unreliable_run(self):
        before = [cpu.estimates(session(97, 70)), cpu.estimates(session(98, 70))]
        after = [cpu.estimates(session(90, 70, errors=50)), cpu.estimates(session(91, 70))]
        self.assertIn("unreliable", cpu.compare(before, after, 1)[0])

    def test_threads_missing_from_a_run(self):
        lone = {"0040": cpu.Thread("0040", 1.0, 10, 0)}
        self.assertEqual(cpu.compare([lone], [cpu.estimates(session(1, 1))], 3),
                         ["no thread appears in every run: nothing to compare"])


class CommandLine(unittest.TestCase):
    def test_runs_and_comparison(self):
        with tempfile.TemporaryDirectory() as directory:
            paths = {}
            for name, main, worker in (("a1", 97, 71.5), ("b1", 94, 69), ("a2", 98, 71.4), ("b2", 95, 69.1)):
                paths[name] = Path(directory) / f"{name}.log"
                paths[name].write_text(session(main, worker))
            out = run(str(paths["a1"]), "--threads", "1")
            self.assertEqual(out, f"{paths['a1']}: tid=0024 97.0 s\n")
            out = run("--baseline", str(paths["a1"]), str(paths["a2"]),
                      "--candidate", str(paths["b1"]), str(paths["b2"]))
            self.assertIn("tid=0024: baseline 97.0, 98.0 s; candidate 94.0, 95.0 s; median -3.1%: faster", out)
            self.assertIn("tid=00b0: baseline 71.5, 71.4 s; candidate 69.0, 69.1 s; median -3.4%: faster", out)
            self.assertNotIn("note:", out)
            out = run("--baseline", str(paths["a1"]), "--candidate", str(paths["b1"]))
            self.assertIn("fewer than two runs per build", out)

    def test_refusals(self):
        with tempfile.TemporaryDirectory() as directory:
            empty = Path(directory) / "empty.log"
            empty.write_text("REC seq=1 t=1 PW_WINE64 session_end reason=wine-exit\n")
            with self.assertRaisesRegex(SystemExit, "no execution lines"):
                run(str(empty))
        for args in ([], ["--baseline", "x.log"], ["--candidate", "x.log"]):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                cpu.main(args)


if __name__ == "__main__":
    unittest.main(verbosity=0)
