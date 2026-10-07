#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Exercise interval boundaries, corruption handling and explicit comparisons."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[1] / "tools/summarize_vk_batch.py"
spec = importlib.util.spec_from_file_location("vk_batch_summary", TOOL)
summary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(summary)


def row(present, base=0, **changes):
    fields = dict(version=1, scope="process", tid=76, present=present, result=0,
                  enabled=1, negotiated=1)
    fields.update({name: base for name in summary.COUNTERS})
    fields.update(changes)
    return "PW_VK_BATCH " + " ".join(f"{k}={v}" for k, v in fields.items())


class SummaryTests(unittest.TestCase):
    def test_exact_intervals_statistics_and_thread_changes(self):
        result = summary.summarize([row(1, 100), row(2, 109, tid=99), row(3, 121)])
        segment = result["segments"][0]
        self.assertEqual(segment["interval_count"], 2)
        self.assertEqual(segment["intervals"][0]["counts"]["records"], 9)
        self.assertEqual(segment["intervals"][0]["end_tid"], 99)
        self.assertEqual(segment["counts_per_present_interval"]["wine_unix_crossings"],
                         dict(samples=2, mean=10.5, median=10.5, min=9, max=12))
        self.assertNotIn("comparison", result)
        self.assertEqual(result["scope"], "process")
        self.assertIn("32-bit winevulkan intercepted UNIX_CALL", result["metric"])
        self.assertTrue(any("64-bit PE winevulkan" in limit for limit in result["limits"]))

    def test_timestamp_prefixes_actual_and_ps5log_escaped(self):
        for separator, ending in [("\t", "\n"), (r"\t", r"\n")]:
            with self.subTest(separator=separator):
                lines = [separator.join(["12", "1000", "INFO", "WINE " + row(1)]) + ending,
                         separator.join(["13", "1025", "INFO", "WINE " + row(2, 10)]) + ending]
                result = summary.summarize(lines)
                self.assertEqual(result["segments"][0]["intervals"][0]["elapsed_ns"], 25)

    def test_boots_never_share_intervals(self):
        r = summary.summarize(["HELLO ps5log/1 boot=0xaa title=PPSA99995", row(1), row(2, 10),
                               "HELLO ps5log/1 boot=0xbb title=PPSA99995", row(1, 200), row(2, 230)])
        self.assertEqual([s["boot"] for s in r["segments"]], ["0xaa", "0xbb"])
        self.assertEqual([s["interval_count"] for s in r["segments"]], [1, 1])
        self.assertNotIn("overall", r)

    def test_repeated_boot_header_is_also_boundary(self):
        r = summary.summarize(["HELLO ps5log/1 boot=a", row(1), "HELLO ps5log/1 boot=a", row(2)])
        self.assertTrue(all(s["interval_count"] == 0 for s in r["segments"]))

    def test_duplicate_regression_gap_and_mode_reset(self):
        for middle, reason in [(row(2, 20), "duplicate or regressing present"),
                               (row(1, 20), "duplicate or regressing present"),
                               (row(4, 20), "missing present snapshots"),
                               (row(3, 20, enabled=0), "mode changed"),
                               (row(3, 20, negotiated=0), "mode changed")]:
            with self.subTest(reason=reason, middle=middle):
                r = summary.summarize([row(1), row(2, 10), middle])
                self.assertEqual(len(r["segments"]), 2)
                self.assertEqual(r["segments"][0]["interval_count"], 1)
                self.assertEqual(r["segments"][1]["interval_count"], 0)
                self.assertEqual(r["issues"][-1]["reason"], reason)

    def test_each_counter_regression_breaks_segment(self):
        for counter in summary.COUNTERS:
            with self.subTest(counter=counter):
                r = summary.summarize([row(1, 50), row(2, 60, **{counter: 49}), row(3, 70)])
                self.assertEqual(r["issues"][0]["reason"], "counter regressed")
                self.assertEqual(r["segments"][0]["interval_count"], 0)
                self.assertEqual(r["segments"][1]["interval_count"], 1)

    def test_failed_present_excludes_both_adjacent_intervals(self):
        r = summary.summarize([row(1), row(2, 10, result=-1000001004), row(3, 20), row(4, 30)])
        self.assertEqual([s["interval_count"] for s in r["segments"]], [0, 1])
        self.assertEqual(r["segments"][1]["intervals"][0]["start_present"], 3)
        self.assertEqual(r["issues"][0]["reason"], "failed present")

    def test_suboptimal_boundary_is_accepted(self):
        r = summary.summarize([row(1), row(2, 10, result=1000001003)])
        self.assertEqual(r["segments"][0]["interval_count"], 1)

    def test_malformed_rows_and_nonprocess_scope_break_segment(self):
        for middle in [row(2, 10, scope="thread"), row(2, 10, version=2),
                       row(2, 10) + " records=1", row(2, 10, enabled=2),
                       row(2, 10, wine_unix_crossings=-1),
                       row(2, 10, records=1 << 64), row(2, 10).replace(" enqueued=10", ""),
                       row(2, 10, tid="0x4c"), row(2, 10) + " " + row(2, 10)]:
            with self.subTest(middle=middle):
                r = summary.summarize([row(1), middle, row(3, 20), row(4, 30)])
                self.assertEqual([s["interval_count"] for s in r["segments"]], [0, 1])
                self.assertEqual(len(r["issues"]), 1)

    def test_unrelated_lines_do_not_break_interval(self):
        r = summary.summarize([row(1), "WINE unrelated info", row(2, 0)])
        self.assertEqual(r["segments"][0]["interval_count"], 1)
        self.assertEqual(r["segments"][0]["intervals"][0]["counts"]["records"], 0)

    def test_timestamp_regression_breaks_segment(self):
        r = summary.summarize(["1\t100\tINFO\t" + row(1), "2\t99\tINFO\t" + row(2, 10)])
        self.assertEqual(r["issues"][0]["reason"], "timestamp did not increase")

    def test_single_snapshot_has_no_counts(self):
        r = summary.summarize([row(100, 123456)])
        self.assertEqual(r["segments"][0]["counts_per_present_interval"], {})

    def test_explicit_comparison_only_and_evidence_requirement(self):
        candidate = summary.summarize([row(1), row(2, 20)])
        baseline = summary.summarize([row(1, enabled=0, negotiated=0), row(2, 100, enabled=0, negotiated=0)])
        c = summary.compare(candidate, baseline, 0, 0, "Matched save/camera/config receipt X")
        self.assertAlmostEqual(c["mean_crossings_reduction_fraction"], .8)
        self.assertNotIn("fps_gain", c)
        for evidence, cs, bs in [("", 0, 0), ("X", -1, 0), ("X", 99, 0)]:
            with self.assertRaises(ValueError):
                summary.compare(candidate, baseline, cs, bs, evidence)
        with self.assertRaises(ValueError):
            summary.compare(candidate, candidate, 0, 0, "X")

    def test_cli_sources_hash_and_no_automatic_comparison(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            first, second, output = p / "one.log", p / "two.log", p / "out.json"
            first.write_text(row(1) + "\n" + row(2, 10) + "\n")
            second.write_text(row(1, 100) + "\n" + row(2, 110) + "\n")
            subprocess.run([sys.executable, str(TOOL), str(first), str(second), "--output", str(output)], check=True)
            r = json.loads(output.read_text())
            self.assertEqual(len(r["logs"]), 2)
            self.assertNotIn("comparison", r)
            self.assertEqual(len(r["logs"][0]["source_sha256"]), 64)
            bad = subprocess.run([sys.executable, str(TOOL), str(first), "--baseline", str(second)],
                                 capture_output=True)
            self.assertNotEqual(bad.returncode, 0)


if __name__ == "__main__":
    unittest.main()
