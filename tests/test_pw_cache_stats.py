#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Cache diagnostics conserve cumulative counters across resets and thread reuse."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from pw_cache_stats import analyze, parse_lines  # noqa: E402


def line(**changes):
    values = dict(tid="0024", instance=1, cumulative=1, time_ns=100, final=0,
                  generation=1, capacity=8, occupied=4, arena_used=64, arena_bytes=1024,
                  hits=8, misses=2, probes=12, max_probe=2, publishes=4, resets=0)
    values.update(changes)
    return "capture prefix wowprospero cache: " + " ".join(f"{k}={v}" for k, v in values.items())


class CacheStats(unittest.TestCase):
    def report(self, *lines):
        records, invalid = parse_lines(lines)
        self.assertEqual(invalid, [])
        return analyze(records)

    def test_deltas_duplicates_and_weighted_probes(self):
        first = line()
        middle = line(time_ns=200, hits=9, probes=22, max_probe=8)
        last = line(time_ns=300, hits=18, probes=31, max_probe=8)
        report = self.report(first, first, middle, last)[0]
        self.assertEqual(report["measured_delta"]["probes"], 19)
        self.assertEqual(report["average_probes"], 1.9)
        self.assertEqual([i["average_probes"] for i in report["intervals"]], [10, 1])
        self.assertEqual(report["invalid_intervals"], [])
        self.assertEqual(report["lifetime_max_probe"], 8)

    def test_reset_keeps_cumulative_counts_and_wraps_generation(self):
        report = self.report(line(generation=0xffffffff),
                             line(time_ns=200, generation=1, occupied=2, arena_used=32,
                                  publishes=7, resets=1, hits=10, misses=5, probes=20))[0]
        self.assertEqual(report["invalid_intervals"], [])
        self.assertEqual(report["measured_delta"]["publishes"], 3)
        self.assertEqual(report["measured_delta"]["resets"], 1)
        self.assertEqual(report["intervals"][0]["occupancy_fraction"], 0.25)

    def test_same_tid_new_instance_is_separate(self):
        reports = self.report(line(), line(instance=2, time_ns=200, hits=1, misses=0,
                                          probes=1, max_probe=1, occupied=1, publishes=1))
        self.assertEqual(len(reports), 2)
        self.assertTrue(all(not r["intervals"] for r in reports))

    def test_invalid_interval_is_not_measured(self):
        for changes in [dict(time_ns=100, hits=9, probes=13), dict(hits=7), dict(capacity=16),
                        dict(generation=2), dict(occupied=3), dict(max_probe=1),
                        dict(hits=9, probes=12), dict(probes=13),
                        dict(resets=1, generation=2, occupied=4, publishes=5)]:
            with self.subTest(changes=changes):
                row = dict(time_ns=200)
                row.update(changes)
                report = self.report(line(), line(**row))[0]
                self.assertEqual(len(report["invalid_intervals"]), 1)
                self.assertEqual(report["measured_delta"]["probes"], 0)

    def test_malformed_or_impossible_rows(self):
        for row in [line(capacity=0), line(occupied=9), line(arena_used=1025),
                    line(probes=9), line(publishes=3), line(hits=-1), line() + " hits=8",
                    line().replace("capacity=8", "capacity=?"), "wowprospero cache: tid=0024"]:
            with self.subTest(row=row):
                records, invalid = parse_lines([row])
                self.assertFalse(records)
                self.assertEqual(len(invalid), 1)
        self.assertEqual(parse_lines(["unrelated log"]), ([], []))


if __name__ == "__main__":
    unittest.main()
