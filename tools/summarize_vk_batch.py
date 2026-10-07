#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Summarize process-scoped Wine Vulkan crossings between logged presents."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import statistics

COUNTERS = ("enqueued", "records", "wine_unix_crossings", "batch_dispatches",
            "piggybacks", "standalone_flushes", "fallback", "arena_full")
SUCCESS_RESULTS = {0, 1000001003}  # VK_SUCCESS, VK_SUBOPTIMAL_KHR
MARKER = "PW_VK_BATCH "
TIMESTAMP = re.compile(r"^\s*\d+(?:\t|\\t)(\d+)(?:\t|\\t)")
MAX_U64 = (1 << 64) - 1


def parse_snapshot(line):
    """None for unrelated lines; raise ValueError for malformed batch markers."""
    if MARKER not in line:
        return None
    if line.count(MARKER) != 1:
        raise ValueError("multiple batch markers")
    fields = {}
    payload = line.split(MARKER, 1)[1].strip()
    # ps5log escapes line endings in payloads; ordinary console logs may use
    # actual newlines. Strip only encoded trailing CR/LF, not field contents.
    while payload.endswith((r"\n", r"\r")):
        payload = payload[:-2]
    for token in payload.split():
        if "=" not in token:
            raise ValueError("invalid field token")
        key, value = token.split("=", 1)
        if key in fields:
            raise ValueError("duplicate field: " + key)
        fields[key] = value
    if fields.get("scope") != "process":
        raise ValueError("scope must be process")
    for key in ("version", "present", "tid", "result", "enabled", "negotiated", *COUNTERS):
        if key not in fields or not re.fullmatch(r"-?\d+", fields[key]):
            raise ValueError("missing or invalid integer: " + key)
        fields[key] = int(fields[key])
    if fields["version"] != 1:
        raise ValueError("unsupported version")
    if fields["enabled"] not in (0, 1) or fields["negotiated"] not in (0, 1):
        raise ValueError("invalid mode")
    if not 1 <= fields["present"] <= MAX_U64 or not 0 <= fields["tid"] <= MAX_U64:
        raise ValueError("invalid present or tid")
    if not -(1 << 31) <= fields["result"] < (1 << 31):
        raise ValueError("invalid VkResult")
    if any(not 0 <= fields[key] <= MAX_U64 for key in COUNTERS):
        raise ValueError("counter outside uint64 range")
    match = TIMESTAMP.match(line)
    fields["timestamp_ns"] = int(match[1]) if match else None
    return fields


def distribution(values):
    return {"samples": len(values), "mean": statistics.mean(values),
            "median": statistics.median(values), "min": min(values), "max": max(values)}


def summarize(lines, source="<memory>"):
    segments, issues = [], []
    boot, current, previous = None, None, None
    snapshots = 0

    def reset():
        nonlocal current, previous
        current, previous = None, None

    def start(row, line_number):
        nonlocal current, previous
        current = {"index": len(segments), "boot": boot,
                   "mode": {"enabled": row["enabled"], "negotiated": row["negotiated"]},
                   "first_line": line_number, "first_present": row["present"],
                   "intervals": []}
        segments.append(current)
        previous = row

    for line_number, line in enumerate(lines, 1):
        if line.startswith("HELLO ps5log/1 "):
            match = re.search(r"(?:^|\s)boot=(\S+)", line)
            boot = match[1] if match else None
            reset()  # Also conservative on repeated headers in the same boot.
            continue
        try:
            row = parse_snapshot(line)
        except ValueError as error:
            issues.append({"line": line_number, "reason": str(error)})
            reset()
            continue
        if row is None:
            continue
        snapshots += 1
        if row["result"] not in SUCCESS_RESULTS:
            issues.append({"line": line_number, "reason": "failed present",
                           "result": row["result"], "present": row["present"]})
            reset()  # Neither interval adjacent to a failed attempt is a frame.
            continue
        if previous is None:
            start(row, line_number)
            continue
        reason = None
        if (row["enabled"], row["negotiated"]) != (previous["enabled"], previous["negotiated"]):
            reason = "mode changed"
        elif row["present"] <= previous["present"]:
            reason = "duplicate or regressing present"
        elif row["present"] != previous["present"] + 1:
            reason = "missing present snapshots"
        elif any(row[key] < previous[key] for key in COUNTERS):
            reason = "counter regressed"
        elif (row["timestamp_ns"] is not None and previous["timestamp_ns"] is not None
              and row["timestamp_ns"] <= previous["timestamp_ns"]):
            reason = "timestamp did not increase"
        if reason:
            issues.append({"line": line_number, "reason": reason})
            reset()
            start(row, line_number)
            continue
        delta = {key: row[key] - previous[key] for key in COUNTERS}
        current["intervals"].append({"start_present": previous["present"],
                                     "end_present": row["present"], "end_line": line_number,
                                     "start_tid": previous["tid"], "end_tid": row["tid"],
                                     "end_result": row["result"], "counts": delta,
                                     "elapsed_ns": (row["timestamp_ns"] - previous["timestamp_ns"]
                                                    if row["timestamp_ns"] is not None
                                                    and previous["timestamp_ns"] is not None else None)})
        previous = row
    for segment in segments:
        intervals = segment["intervals"]
        segment["interval_count"] = len(intervals)
        segment["counts_per_present_interval"] = {
            key: distribution([item["counts"][key] for item in intervals])
            for key in COUNTERS} if intervals else {}
    return {"source": source, "schema": "pw-vk-batch-summary/1", "scope": "process",
            "metric": "Wine Vulkan Unix calls between consecutive successful/suboptimal present attempts",
            "snapshots": snapshots, "segments": segments, "issues": issues,
            "limits": ["wine_unix_crossings excludes non-winevulkan Unix calls and native FS transitions",
                       "records counts replayed commands; enqueued counts accepted deferred commands",
                       "Present attempts are process-scoped, not per swapchain or thread; no displayed-frame or FPS claim",
                       "No aggregation across boots, modes, gaps, failures, malformed rows or counter resets",
                       "A single snapshot cannot determine per-present counts; initialization before first snapshot is excluded"]}


def read_log(path):
    data = path.read_bytes()
    result = summarize(data.decode("utf-8", errors="replace").splitlines(), str(path.resolve()))
    result["source_sha256"] = hashlib.sha256(data).hexdigest()
    return result


def compare(candidate, baseline, candidate_segment, baseline_segment, evidence):
    """Caller must explicitly select equivalent workload segments and evidence."""
    if not evidence.strip():
        raise ValueError("equivalent workload evidence is required")
    try:
        c, b = candidate["segments"][candidate_segment], baseline["segments"][baseline_segment]
    except IndexError as error:
        raise ValueError("selected segment does not exist") from error
    if candidate_segment < 0 or baseline_segment < 0:
        raise ValueError("segment indices must be nonnegative")
    if not c["interval_count"] or not b["interval_count"]:
        raise ValueError("selected segments need consecutive present intervals")
    if c["mode"] != {"enabled": 1, "negotiated": 1} or b["mode"]["enabled"] != 0:
        raise ValueError("comparison requires negotiated batching versus explicitly disabled baseline")
    cmean = c["counts_per_present_interval"]["wine_unix_crossings"]["mean"]
    bmean = b["counts_per_present_interval"]["wine_unix_crossings"]["mean"]
    if bmean == 0:
        raise ValueError("baseline mean crossing count is zero")
    return {"equivalent_workload_evidence": evidence, "evidence_status": "caller supplied; not validated by parser",
            "candidate_segment": candidate_segment, "baseline_segment": baseline_segment,
            "candidate_mean_wine_crossings_per_present_interval": cmean,
            "baseline_mean_wine_crossings_per_present_interval": bmean,
            "mean_crossings_reduction_fraction": (bmean - cmean) / bmean,
            "limit": "Crossing-count comparison only; does not establish frame-time or FPS improvement"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--equivalent-workload-evidence")
    parser.add_argument("--segment", type=int)
    parser.add_argument("--baseline-segment", type=int)
    args = parser.parse_args()
    comparison_options = (args.baseline, args.equivalent_workload_evidence, args.segment, args.baseline_segment)
    if any(value is not None for value in comparison_options):
        if len(args.logs) != 1 or any(value is None for value in comparison_options):
            parser.error("comparison needs one candidate, --baseline, --equivalent-workload-evidence, --segment and --baseline-segment")
    try:
        output = {"logs": [read_log(path) for path in args.logs]}
        if args.baseline is not None:
            baseline = read_log(args.baseline)
            output["baseline"] = baseline
            output["comparison"] = compare(output["logs"][0], baseline, args.segment,
                                           args.baseline_segment, args.equivalent_workload_evidence)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    text = json.dumps(output, indent=2) + "\n"
    if args.output:
        args.output.write_text(text)
    else:
        print(text, end="")


if __name__ == "__main__":
    main()
