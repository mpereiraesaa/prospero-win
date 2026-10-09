#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Which frames spike, and where the CS thread's time went in them.

Reads a game's log with the per-frame profile on (PW_NATIVE_PROFILE=1,
PW_VK_RADV_PROFILE=1 and, with the DXVK 2.6.2-prospero2 build,
DXVK_CS_PROFILE=1) and joins, per frame:

  PW_VK_RADV_PRESENT    the interval between presents: the frame's length
  PW_NATIVE_PROFILE v3  each native thread's host time by bucket (Unix
                        calls, syscalls, other, FS switches; the rest is
                        guest code) since its previous frame line
  PW_VK_RADV_PROFILE    each thread's time in RADV, by entry point
  PW_VK_RADV_EVENT      single calls over the event threshold
  DXVK_CS_PROFILE       DXVK's CS thread: busy time and time by command

Frames at or over --spike-ms (25) are spikes; frames at or under
--normal-ms (20) are normal. The report gives the frame-length
distribution, then for the CS thread (the native thread with the most Unix
calls per frame, or --tid) the mean per-frame split in normal and spike
frames, the RADV entry points and DXVK commands that grow most in spikes,
and the events that fell in spike frames. --csv writes one row per frame.

Usage: native_profile_frames.py LOG [--spike-ms MS] [--normal-ms MS] [--tid HEX]
                                [--tsc-hz HZ] [--csv FILE] [--top N]
"""
from __future__ import annotations

import argparse
import csv
import re
import statistics
import sys
from collections import defaultdict
from dataclasses import dataclass, field

FIELD = re.compile(r"(\w+)=(-?[0-9a-fA-F]+)(?:/(\d+))?(?=\s|$)")
NATIVE = re.compile(r"PW_NATIVE_PROFILE version=3 (.*)$")
PRESENT = re.compile(r"PW_VK_RADV_PRESENT version=1 (.*)$")
RADV = re.compile(r"PW_VK_RADV_PROFILE version=1 (frame=.*)$")
RADV_START = re.compile(r"PW_VK_RADV_PROFILE version=1 start (.*)$")
EVENT = re.compile(r"PW_VK_RADV_EVENT version=1 (.*)$")
DXVK = re.compile(r"DXVK_CS_PROFILE version=1 (frame=.*)$")
DXVK_START = re.compile(r"DXVK_CS_PROFILE version=1 start (.*)$")
DXVK_NAME = re.compile(r"DXVK_CS_PROFILE name id=(\d+) text=(.*)$")
HEX_FIELDS = {"tid"}
NATIVE_BUCKETS = ("guest", "unix", "syscall", "other", "fs")


def fields(text: str) -> dict:
    """name=value pairs; name=calls/ticks pairs become (calls, ticks)."""
    out = {}
    for key, value, ticks in FIELD.findall(text):
        try:
            if ticks:
                out[key] = (int(value), int(ticks))
            else:
                out[key] = int(value, 16 if key in HEX_FIELDS else 10)
        except ValueError:
            continue    # a word that happens to spell hex digits, like a name
    return out


@dataclass
class Log:
    tsc_hz: int = 0
    presents: dict = field(default_factory=dict)          # frame -> {tsc, interval, ticks, result, thread}
    native: dict = field(default_factory=lambda: defaultdict(list))   # tid -> [fields]
    radv: dict = field(default_factory=lambda: defaultdict(list))     # thread -> [(fields, {fn: (calls, ticks)})]
    events: list = field(default_factory=list)            # [(fields, fn, detail)]
    dxvk: list = field(default_factory=list)              # [(fields, {id: (calls, ticks)})]
    dxvk_names: dict = field(default_factory=dict)        # id -> text
    event_us: int = 0


def parse(lines) -> Log:
    log = Log()
    for line in lines:
        if (m := NATIVE.search(line)):
            f = fields(m[1])
            if "tid" in f and "frame" in f:
                log.native[f["tid"]].append(f)
                if not log.tsc_hz and f.get("tsc_hz"):
                    log.tsc_hz = f["tsc_hz"]
        elif (m := PRESENT.search(line)):
            f = fields(m[1])
            if "frame" in f:
                log.presents[f["frame"]] = f
        elif (m := RADV_START.search(line)):
            f = fields(m[1])
            if not log.tsc_hz and f.get("tsc_hz"):
                log.tsc_hz = f["tsc_hz"]
            log.event_us = f.get("event_us", 0)
        elif (m := RADV.search(line)):
            f = fields(m[1])
            if "frame" in f and "thread" in f:
                per_fn = {k: v for k, v in f.items() if isinstance(v, tuple)}
                log.radv[f["thread"]].append((f, per_fn))
        elif (m := EVENT.search(line)):
            f = fields(m[1])
            fn = re.search(r"fn=(\w+)", m[1])
            detail = m[1].split(f"ticks={f.get('ticks', 0)}", 1)[-1].strip() if "ticks" in f else ""
            if fn and "frame" in f:
                log.events.append((f, fn[1], detail))
        elif (m := DXVK_NAME.search(line)):
            log.dxvk_names[int(m[1])] = m[2].strip()
        elif (m := DXVK_START.search(line)):
            f = fields(m[1])
            if not log.tsc_hz and f.get("tsc_hz"):
                log.tsc_hz = f["tsc_hz"]
        elif (m := DXVK.search(line)):
            f = fields(m[1])
            if "frame" in f:
                per_id = {int(k[1:]): v for k, v in f.items() if isinstance(v, tuple) and k.startswith("c")}
                log.dxvk.append((f, per_id))
    return log


def align_dxvk(log: Log) -> int:
    """The offset from DXVK's frame ids to the driver's present numbers.

    DXVK's CS thread writes frame F when it hands present F to the submit
    thread, before the driver's present F returns, so DXVK's line comes
    just before the present line with the same number when both count the
    same presents. The offset is the median over the lines of (number of
    the first present returned after the line's TSC) minus (the line's
    frame id); 0 when nothing can be aligned."""
    if not log.dxvk or not log.presents:
        return 0
    presents = sorted((p["tsc"], frame) for frame, p in log.presents.items() if "tsc" in p)
    if not presents:
        return 0
    offsets = []
    for f, _ in log.dxvk:
        if "tsc" not in f:
            continue
        index = next((i for i, (tsc, _) in enumerate(presents) if tsc >= f["tsc"]), None)
        if index is not None:
            offsets.append(presents[index][1] - f["frame"])
    return int(statistics.median(offsets)) if offsets else 0


def frame_ms(log: Log, hz: int) -> dict:
    """frame -> its length in ms (the present interval ending at it)."""
    return {frame: p["interval"] * 1000 / hz for frame, p in log.presents.items() if p.get("interval")}


def classify(lengths: dict, spike_ms: float, normal_ms: float) -> tuple[set, set]:
    spikes = {f for f, ms in lengths.items() if ms >= spike_ms}
    normal = {f for f, ms in lengths.items() if ms <= normal_ms}
    return normal, spikes


def percentile(values: list, fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, int(round(fraction * (len(ordered) - 1))))
    return ordered[index]


def cs_tid(log: Log) -> int | None:
    """The thread with the most Unix calls per frame line."""
    best, best_rate = None, -1.0
    for tid, rows in log.native.items():
        single = [r for r in rows if r.get("frames") == 1]
        if len(single) < 10:
            continue
        rate = sum(r.get("unix_calls", 0) for r in single) / len(single)
        if rate > best_rate:
            best, best_rate = tid, rate
    return best


def native_split(rows: list, frames: set, hz: int) -> dict:
    """Mean ms per bucket over the single-frame lines ending in frames."""
    picked = [r for r in rows if r.get("frames") == 1 and r["frame"] in frames]
    out = {"frames": len(picked)}
    if not picked:
        return out
    for bucket in ("wall", "unix", "syscall", "other", "fs"):
        out[bucket] = statistics.mean(r.get(bucket, 0) for r in picked) * 1000 / hz
    out["guest"] = max(0.0, out["wall"] - out["unix"] - out["syscall"] - out["other"] - out["fs"])
    out["unix_calls"] = statistics.mean(r.get("unix_calls", 0) for r in picked)
    out["syscall_calls"] = statistics.mean(r.get("syscall_calls", 0) for r in picked)
    return out


def radv_thread_for(log: Log, tid: int) -> int | None:
    """The RADV thread number that rolled over at the same TSC as tid's lines."""
    marks = {(r["frame"], r["tsc"]) for r in log.native.get(tid, [])}
    for thread, rows in log.radv.items():
        if any((f["frame"], f["tsc"]) in marks for f, _ in rows):
            return thread
    return None


def per_function(rows: list, frames: set, hz: int) -> dict:
    """fn -> (mean calls per frame, mean ms per frame) over single-frame lines in frames."""
    picked = [(f, per_fn) for f, per_fn in rows if f.get("frames") == 1 and f["frame"] in frames]
    totals: dict = defaultdict(lambda: [0, 0])
    for _, per_fn in picked:
        for fn, (calls, ticks) in per_fn.items():
            totals[fn][0] += calls
            totals[fn][1] += ticks
    count = len(picked) or 1
    return {fn: (calls / count, ticks * 1000 / hz / count) for fn, (calls, ticks) in totals.items()}


def ms(ticks: int, hz: int) -> float:
    return ticks * 1000 / hz


def report(log: Log, spike_ms: float, normal_ms: float, tid: int | None, top: int, out=sys.stdout) -> int:
    hz = log.tsc_hz
    if not hz:
        print("no TSC rate in the log (PW_NATIVE_PROFILE or PW_VK_RADV_PROFILE start); pass --tsc-hz", file=out)
        return 1
    lengths = frame_ms(log, hz)
    if not lengths:
        print("no PW_VK_RADV_PRESENT lines with an interval: was PW_VK_RADV_PROFILE=1 (or PW_NATIVE_PROFILE=1) set?",
              file=out)
        return 1
    normal, spikes = classify(lengths, spike_ms, normal_ms)
    values = list(lengths.values())
    print(f"frames: {len(values)}  tsc_hz={hz}  event threshold {log.event_us} us", file=out)
    print(f"frame length ms: p50 {percentile(values, 0.5):.1f}  p90 {percentile(values, 0.9):.1f}  "
          f"p99 {percentile(values, 0.99):.1f}  max {max(values):.1f}  "
          f">25 ms {sum(v > 25 for v in values)}  >33 ms {sum(v > 33 for v in values)}  "
          f">50 ms {sum(v > 50 for v in values)}", file=out)
    print(f"normal (<= {normal_ms:g} ms): {len(normal)}   spikes (>= {spike_ms:g} ms): {len(spikes)}", file=out)
    if spikes:
        worst = sorted(spikes, key=lambda f: (-lengths[f], f))[:10]
        print("worst frames: " + ", ".join(f"#{f} {lengths[f]:.1f} ms" for f in worst), file=out)

    if tid is None:
        tid = cs_tid(log)
    if tid is None or tid not in log.native:
        print("no native per-frame lines (PW_NATIVE_PROFILE=1 with the frame hook): no thread split", file=out)
    else:
        rows = log.native[tid]
        multi = sum(1 for r in rows if r.get("frames", 1) != 1)
        n, s = native_split(rows, normal, hz), native_split(rows, spikes, hz)
        print(f"\nCS thread tid={tid:04x}: {len(rows)} frame lines ({multi} spanning several frames, left out)",
              file=out)
        print(f"{'bucket':>10} {'normal ms':>10} {'spike ms':>10} {'delta ms':>10}   ({n['frames']} normal, "
              f"{s['frames']} spike frames)", file=out)
        for bucket in ("wall",) + NATIVE_BUCKETS:
            a, b = n.get(bucket, 0.0), s.get(bucket, 0.0)
            print(f"{bucket:>10} {a:>10.2f} {b:>10.2f} {b - a:>+10.2f}", file=out)
        print(f"{'unix calls':>10} {n.get('unix_calls', 0):>10.0f} {s.get('unix_calls', 0):>10.0f}", file=out)
        print(f"{'syscalls':>10} {n.get('syscall_calls', 0):>10.0f} {s.get('syscall_calls', 0):>10.0f}", file=out)
        thread = radv_thread_for(log, tid)
        if thread is None:
            print("no RADV lines joined to this thread (PW_VK_RADV_PROFILE=1?)", file=out)
        else:
            rn, rs = per_function(log.radv[thread], normal, hz), per_function(log.radv[thread], spikes, hz)
            names = sorted(set(rn) | set(rs), key=lambda fn: -(rs.get(fn, (0, 0))[1]))
            print(f"\nRADV entry points on the CS thread (RADV thread {thread}), ms per frame:", file=out)
            print(f"{'function':<40} {'normal':>8} {'spike':>8} {'delta':>8} {'calls n':>8} {'calls s':>8}",
                  file=out)
            for fn in names[:top]:
                a, b = rn.get(fn, (0, 0)), rs.get(fn, (0, 0))
                print(f"{fn:<40} {a[1]:>8.3f} {b[1]:>8.3f} {b[1] - a[1]:>+8.3f} {a[0]:>8.0f} {b[0]:>8.0f}",
                      file=out)
            total_n = sum(v[1] for v in rn.values())
            total_s = sum(v[1] for v in rs.values())
            print(f"{'total':<40} {total_n:>8.3f} {total_s:>8.3f} {total_s - total_n:>+8.3f}", file=out)

    others = [t for t in log.radv if tid is None or t != radv_thread_for(log, tid)]
    for thread in sorted(others):
        rs = per_function(log.radv[thread], spikes, hz)
        rn = per_function(log.radv[thread], normal, hz)
        total_n = sum(v[1] for v in rn.values())
        total_s = sum(v[1] for v in rs.values())
        if total_n or total_s:
            busiest = sorted(set(rn) | set(rs), key=lambda fn: -(rs.get(fn, (0, 0))[1]))[:3]
            print(f"RADV thread {thread}: {total_n:.3f} ms/frame normal, {total_s:.3f} spike; most in "
                  + ", ".join(busiest), file=out)

    if log.events:
        in_spikes = [e for e in log.events if e[0]["frame"] in spikes]
        by_fn: dict = defaultdict(lambda: [0, 0])
        for f, fn, _ in log.events:
            by_fn[fn][0] += 1
            by_fn[fn][1] += f.get("ticks", 0)
        print(f"\nevents over {log.event_us} us: {len(log.events)} total, {len(in_spikes)} in spike frames", file=out)
        for fn, (count, ticks) in sorted(by_fn.items(), key=lambda item: -item[1][1])[:top]:
            print(f"  {fn:<40} {count:>6} calls {ms(ticks, hz):>9.2f} ms total {ms(ticks, hz) / count:>8.3f} ms mean",
                  file=out)
        slowest = sorted(log.events, key=lambda e: -e[0].get("ticks", 0))[:top]
        print("slowest events:", file=out)
        for f, fn, detail in slowest:
            tag = "spike" if f["frame"] in spikes else ("normal" if f["frame"] in normal else "other")
            print(f"  frame {f['frame']} ({tag}, {lengths.get(f['frame'], 0):.1f} ms) thread {f.get('thread', 0)} "
                  f"{fn} {ms(f.get('ticks', 0), hz):.3f} ms {detail}", file=out)

    if log.dxvk:
        offset = align_dxvk(log)
        dxvk = [({**f, "frame": f["frame"] + offset}, c) for f, c in log.dxvk]
        dn = [(f, c) for f, c in dxvk if f.get("frames", 1) == 1 and f["frame"] in normal]
        ds = [(f, c) for f, c in dxvk if f.get("frames", 1) == 1 and f["frame"] in spikes]

        def mean_field(rows, key):
            return statistics.mean(f.get(key, 0) for f, _ in rows) * 1000 / hz if rows else 0.0

        print(f"\nDXVK CS thread: {len(log.dxvk)} frame lines (frame id + {offset} = present), {len(dn)} normal, "
              f"{len(ds)} spike", file=out)
        for key in ("wall", "busy"):
            print(f"  {key:>6} ms: normal {mean_field(dn, key):.2f}  spike {mean_field(ds, key):.2f}", file=out)
        print(f"  commands per frame: normal {statistics.mean(f.get('cmds', 0) for f, _ in dn) if dn else 0:.0f}  "
              f"spike {statistics.mean(f.get('cmds', 0) for f, _ in ds) if ds else 0:.0f}", file=out)

        def per_cmd(rows):
            totals: dict = defaultdict(lambda: [0, 0])
            for _, per_id in rows:
                for cmd, (calls, ticks) in per_id.items():
                    totals[cmd][0] += calls
                    totals[cmd][1] += ticks
            count = len(rows) or 1
            return {cmd: (calls / count, ticks * 1000 / hz / count) for cmd, (calls, ticks) in totals.items()}

        cn, cs = per_cmd(dn), per_cmd(ds)
        print(f"{'command':<56} {'normal':>8} {'spike':>8} {'delta':>8} {'calls n':>8} {'calls s':>8}", file=out)
        for cmd in sorted(set(cn) | set(cs), key=lambda c: -(cs.get(c, (0, 0))[1]))[:top]:
            a, b = cn.get(cmd, (0, 0)), cs.get(cmd, (0, 0))
            name = log.dxvk_names.get(cmd, f"command {cmd}")
            print(f"{name[:56]:<56} {a[1]:>8.3f} {b[1]:>8.3f} {b[1] - a[1]:>+8.3f} {a[0]:>8.0f} {b[0]:>8.0f}",
                  file=out)
    return 0


def write_csv(log: Log, path: str, tid: int | None) -> None:
    hz = log.tsc_hz
    if tid is None:
        tid = cs_tid(log)
    native = {r["frame"]: r for r in log.native.get(tid, []) if r.get("frames") == 1} if tid is not None else {}
    thread = radv_thread_for(log, tid) if tid is not None else None
    radv = {f["frame"]: (f, per_fn) for f, per_fn in log.radv.get(thread, []) if f.get("frames") == 1}
    offset = align_dxvk(log)
    dxvk = {f["frame"] + offset: f for f, _ in log.dxvk if f.get("frames", 1) == 1}
    with open(path, "w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["frame", "length_ms", "wall_ms", "guest_ms", "unix_ms", "syscall_ms", "other_ms", "fs_ms",
                         "unix_calls", "radv_ms", "radv_calls", "dxvk_busy_ms", "dxvk_cmds"])
        for frame in sorted(log.presents):
            p = log.presents[frame]
            row = [frame, f"{p.get('interval', 0) * 1000 / hz:.3f}"]
            r = native.get(frame)
            if r:
                wall = r.get("wall", 0)
                rest = sum(r.get(k, 0) for k in ("unix", "syscall", "other", "fs"))
                row += [f"{ms(wall, hz):.3f}", f"{ms(max(0, wall - rest), hz):.3f}"]
                row += [f"{ms(r.get(k, 0), hz):.3f}" for k in ("unix", "syscall", "other", "fs")]
                row.append(r.get("unix_calls", 0))
            else:
                row += [""] * 7
            f = radv.get(frame)
            row += [f"{ms(f[0].get('ticks', 0), hz):.3f}", f[0].get("calls", 0)] if f else ["", ""]
            d = dxvk.get(frame)
            row += [f"{ms(d.get('busy', 0), hz):.3f}", d.get("cmds", 0)] if d else ["", ""]
            writer.writerow(row)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("log")
    parser.add_argument("--spike-ms", type=float, default=25.0)
    parser.add_argument("--normal-ms", type=float, default=20.0)
    parser.add_argument("--tid", type=lambda text: int(text, 16), help="the CS thread's Windows thread id, hex")
    parser.add_argument("--tsc-hz", type=int, default=0)
    parser.add_argument("--csv")
    parser.add_argument("--top", type=int, default=15)
    args = parser.parse_args(argv)
    with open(args.log, errors="replace") as handle:
        log = parse(handle)
    if args.tsc_hz:
        log.tsc_hz = args.tsc_hz
    status = report(log, args.spike_ms, args.normal_ms, args.tid, args.top)
    if args.csv and log.tsc_hz and log.presents:
        write_csv(log, args.csv, args.tid)
    return status


if __name__ == "__main__":
    sys.exit(main())
