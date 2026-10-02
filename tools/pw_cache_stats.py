#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Analyze owner-thread cache reports from one process; never sum cumulative rows."""
from __future__ import annotations

import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path

COUNTERS = ("hits", "misses", "probes", "publishes", "resets")
FIELDS = ("instance", "cumulative", "time_ns", "final", "generation", "capacity",
          "occupied", "arena_used", "arena_bytes", "max_probe", *COUNTERS)


def parse_lines(lines):
    records, invalid = [], []
    for number, line in enumerate(lines, 1):
        if "wowprospero cache:" not in line:
            continue
        try:
            pairs = [word.split("=", 1) for word in line.split("wowprospero cache:", 1)[1].split()]
            values = dict(pairs)
            if len(values) != len(pairs):
                raise ValueError("duplicate field")
            row = {key: int(values[key], 10) for key in FIELDS}
            row["tid"] = int(values["tid"], 16)
            if any(value < 0 for value in row.values()):
                raise ValueError("negative value")
            if row["cumulative"] != 1 or row["final"] not in (0, 1):
                raise ValueError("unsupported report type")
            if not row["instance"] or not row["generation"] or not row["capacity"]:
                raise ValueError("zero identity or capacity")
            if row["occupied"] > row["capacity"] or row["max_probe"] > row["capacity"]:
                raise ValueError("table bounds")
            if row["arena_used"] > row["arena_bytes"]:
                raise ValueError("arena bounds")
            if row["probes"] < row["hits"] + row["misses"]:
                raise ValueError("fewer probes than lookups")
            if row["occupied"] > row["publishes"]:
                raise ValueError("more occupied entries than publishes")
            row["line"] = number
            records.append(row)
        except (KeyError, ValueError) as error:
            invalid.append({"line": number, "reason": str(error)})
    return records, invalid


def analyze(records):
    groups = defaultdict(list)
    for row in records:
        groups[row["tid"], row["instance"]].append(row)
    results = []
    for (tid, instance), rows in groups.items():
        intervals, invalid = [], []
        for previous, current in zip(rows, rows[1:]):
            if all(previous[key] == current[key] for key in (*FIELDS, "tid")):
                continue  # duplicated capture line
            delta = {key: current[key] - previous[key] for key in COUNTERS}
            reason = None
            if current["time_ns"] <= previous["time_ns"]:
                reason = "non-increasing clock"
            elif any(value < 0 for value in delta.values()):
                reason = "counter regression; do not combine process lifetimes"
            elif any(current[key] != previous[key] for key in ("capacity", "arena_bytes")):
                reason = "capacity changed within one instance"
            elif current["max_probe"] < previous["max_probe"]:
                reason = "lifetime max_probe decreased"
            elif not delta["resets"] and (
                current["generation"] != previous["generation"] or
                current["occupied"] - previous["occupied"] != delta["publishes"]
            ):
                reason = "occupancy/generation changed without reset"
            elif delta["resets"] and current["occupied"] > delta["publishes"]:
                reason = "occupancy exceeds publishes after reset"
            lookups = delta["hits"] + delta["misses"]
            if delta["probes"] < lookups or (not lookups and delta["probes"]):
                reason = "inconsistent probe delta"
            if reason:
                invalid.append({"from_line": previous["line"], "to_line": current["line"],
                                "reason": reason})
                continue
            intervals.append({"from_line": previous["line"], "to_line": current["line"],
                              "elapsed_ns": current["time_ns"] - previous["time_ns"],
                              "delta": delta, "lookups": lookups,
                              "average_probes": delta["probes"] / lookups if lookups else None,
                              "occupancy_fraction": current["occupied"] / current["capacity"],
                              "lifetime_max_probe": current["max_probe"]})
        totals = {key: sum(interval["delta"][key] for interval in intervals) for key in COUNTERS}
        lookups = totals["hits"] + totals["misses"]
        results.append({"tid": f"{tid:04x}", "instance": instance, "records": len(rows),
                        "intervals": intervals, "invalid_intervals": invalid,
                        "measured_delta": totals,
                        "average_probes": totals["probes"] / lookups if lookups else None,
                        "peak_observed_occupancy": max(row["occupied"] / row["capacity"] for row in rows),
                        "lifetime_max_probe": max(row["max_probe"] for row in rows)})
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    raw = args.log.read_bytes()
    records, invalid = parse_lines(raw.decode("utf-8", errors="replace").splitlines())
    result = {"log": str(args.log), "sha256": hashlib.sha256(raw).hexdigest(),
              "threads": analyze(records), "invalid_records": invalid,
              "limits": "One process per input. Counters describe lookups, not CPU time or frames. "
                        "Probe counts cover lookups only, excluding publish and compile chain-patch walks. "
                        "max_probe is a lifetime maximum. No observations before the first report."}
    output = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(output)
    else:
        print(output, end="")
    return 1 if not records or invalid or any(thread["invalid_intervals"] for thread in result["threads"]) else 0


if __name__ == "__main__":
    raise SystemExit(main())
