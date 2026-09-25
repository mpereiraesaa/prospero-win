#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Validate one ps5log/1 prospero-win runtime transcript."""
from __future__ import annotations
import argparse
from pathlib import Path


def number(text: str) -> int:
    return int(text, 0)


def parse(path: Path) -> tuple[list[tuple[int, int, str, dict[str, str]]], str | None]:
    records: list[tuple[int, int, str, dict[str, str]]] = []
    bye = None
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines or not lines[0].startswith("HELLO ps5log/1 title=PPSA99995 app=prospero-win "):
        raise ValueError("missing exact ps5log/1 HELLO identity")
    for line in lines[1:]:
        if line.startswith("BYE "):
            if bye is not None:
                raise ValueError("duplicate BYE")
            bye = line
            continue
        fields = line.split("\t", 3)
        if len(fields) != 4:
            raise ValueError("malformed record")
        seq, stamp = int(fields[0]), int(fields[1])
        words = fields[3].split()
        if not words:
            raise ValueError("empty record")
        values = dict(word.split("=", 1) for word in words[1:] if "=" in word)
        records.append((seq, stamp, words[0], values))
    if [item[0] for item in records] != list(range(1, len(records) + 1)):
        raise ValueError("sequence gap")
    return records, bye


def latest(records, name: str):
    matches = [item for item in records if item[2] == name]
    if not matches:
        raise ValueError(f"missing {name}")
    return matches[-1]


def validate(path: Path, continuous: bool, min_seconds: float,
             min_flips: int, min_audio_blocks: int, min_pad_events: int = 0,
             require_pad_quit: bool = False,
             present_backend: str | None = None) -> None:
    records, bye = parse(path)
    names = [item[2] for item in records]
    if "PW_RUNTIME_ABORT" in names or "PW_RUNTIME_SIGNAL" in names:
        raise ValueError("runtime abort/signal present")
    latest(records, "PW_RUNTIME_BEGIN"); latest(records, "PW_RUNTIME_READY")
    state = latest(records, "PW_STATE_LOAD")[3]
    if state.get("status") != "ok" or number(state.get("bytes", "0")) <= 0:
        raise ValueError("persistent state was not loaded")
    pad = latest(records, "PW_PAD_OPEN")[3]
    if number(pad.get("handle", "-1")) < 0 or pad.get("read") != "scePadRead":
        raise ValueError("pad backend not open")
    heartbeat = latest(records, "PW_RUNTIME_HEARTBEAT")[3]
    required_nonzero = ("retired", "calls", "pad_samples", "pad_connected",
                        "profile_lookups", "profile_bytes", "idle_yields")
    for key in required_nonzero:
        if number(heartbeat.get(key, "0")) <= 0:
            raise ValueError(f"heartbeat {key} is zero")
    for key in ("pad_read_errors", "profile_missing", "profile_errors"):
        if number(heartbeat.get(key, "-1")) != 0:
            raise ValueError(f"heartbeat {key} is nonzero")
    if number(heartbeat.get("schema", "1")) >= 2:
        for key in ("dbt_dispatches", "dbt_compiles", "dbt_hits", "dbt_misses",
                    "dbt_lookup_probes", "dbt_max_probe", "dbt_protect_calls",
                    "dbt_protect_bytes", "audio_enqueues", "audio_completions",
                    "loop_gap_max_ns"):
            if number(heartbeat.get(key, "0")) <= 0:
                raise ValueError(f"async heartbeat {key} is zero")
        for key in ("audio_queue_full", "audio_errors"):
            if number(heartbeat.get(key, "-1")) != 0:
                raise ValueError(f"async heartbeat {key} is nonzero")
        queue = latest(records, "PW_AUDIO_QUEUE")[3]
        if number(queue.get("worker", "0")) != 1:
            raise ValueError("audio worker is not running")
        if number(queue.get("enqueues", "0")) <= 0 or number(queue.get("completions", "0")) <= 0:
            raise ValueError("audio queue made no asynchronous progress")
        if number(queue.get("full", "-1")) != 0 or number(queue.get("output_errors", "-1")) != 0:
            raise ValueError("audio queue reported backpressure/output failure")
    if number(heartbeat.get("pad_events", "0")) < min_pad_events:
        raise ValueError("insufficient physical pad events")
    if require_pad_quit:
        quit_record = latest(records, "PW_PAD_QUIT")[3]
        if quit_record.get("source") != "create" or quit_record.get("action") != "WM_QUIT":
            raise ValueError("invalid pad quit record")
    if len(records) < 2 or (records[-1][1] - records[0][1]) < int(min_seconds * 1e9):
        raise ValueError("run too short")
    if continuous:
        if bye is not None or "PW_RUNTIME_END" in names:
            raise ValueError("continuous run unexpectedly ended")
        counters = heartbeat
    else:
        teardown = latest(records, "PW_RUNTIME_TEARDOWN")[3]
        for key in ("state", "pad", "audio", "gdi", "video", "agc", "dbt",
                    "image", "stack", "thread", "crt", "heap"):
            if teardown.get(key) != "ok":
                raise ValueError(f"teardown {key} is not ok")
        if "session" in teardown:
            session = number(teardown.get("session", "-1"))
            expected_outcome = 1 if session else 0
            if number(teardown.get("outcome", "-1")) != expected_outcome:
                raise ValueError("runtime supervisor outcome does not match session")
            if number(teardown.get("supervisor_state", "-1")) != 0:
                raise ValueError("runtime supervisor did not return to IDLE")
            if teardown.get("supervisor_cleanup") != "ok":
                raise ValueError("runtime supervisor cleanup is not ok")
        end = latest(records, "PW_RUNTIME_END")[3]
        if bye is None or f"reason={end.get('reason')}" not in bye:
            raise ValueError("missing/mismatched BYE")
        # The final partial heartbeat interval belongs in the orderly end
        # record.  Using it avoids under-reporting short bounded validations.
        counters = end
    if number(counters.get("flips", "0")) < min_flips:
        raise ValueError("insufficient flips")
    if present_backend == "vk-wsi":
        validate_vk_wsi(records, names, None if continuous else teardown, counters)
    if number(counters.get("audio_blocks", "0")) < min_audio_blocks:
        raise ValueError("insufficient audio blocks")


def validate_vk_wsi(records, names, teardown, counters) -> None:
    """ps5-vulkan WSI presentation: opened, never failed, every sampled frame
    presented through it with strictly increasing tokens over both images."""
    opened = latest(records, "PW_PRESENT_OPEN")[3]
    if opened.get("backend") != "vk-wsi" or opened.get("status") != "ok":
        raise ValueError("vk-wsi presentation was not opened")
    if "PW_PRESENT_FAIL" in names:
        raise ValueError("vk-wsi presentation failure recorded")
    frames = [item[3] for item in records if item[2] == "PW_VIDEO_FRAME"]
    if not frames or any(frame.get("backend") != "vk-wsi" for frame in frames):
        raise ValueError("frames were not presented through vk-wsi")
    tokens = [number(frame.get("token", "0")) for frame in frames]
    if any(token != number(frame.get("flips", "-1")) for token, frame in zip(tokens, frames)) or \
            any(later <= earlier for earlier, later in zip(tokens, tokens[1:])):
        raise ValueError("vk-wsi frame tokens are not the strictly increasing flip count")
    if len(frames) > 1 and {frame.get("slot") for frame in frames} != {"0", "1"}:
        raise ValueError("vk-wsi frames did not use both swapchain images")
    if teardown is not None:
        if teardown.get("present_backend") != "vk-wsi" or \
                teardown.get("present_failed_call") != "none":
            raise ValueError("vk-wsi teardown is not clean")
        if number(teardown.get("present_flips", "-1")) != number(counters.get("flips", "0")):
            raise ValueError("vk-wsi teardown flips do not match the end record")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("transcript", type=Path)
    parser.add_argument("--continuous", action="store_true")
    parser.add_argument("--min-seconds", type=float, default=0)
    parser.add_argument("--min-flips", type=int, default=1)
    parser.add_argument("--min-audio-blocks", type=int, default=1)
    parser.add_argument("--min-pad-events", type=int, default=0)
    parser.add_argument("--require-pad-quit", action="store_true")
    parser.add_argument("--present-backend", choices=("vk-wsi",))
    args = parser.parse_args()
    try:
        validate(args.transcript, args.continuous, args.min_seconds,
                 args.min_flips, args.min_audio_blocks, args.min_pad_events,
                 args.require_pad_quit, args.present_backend)
    except (OSError, ValueError) as error:
        raise SystemExit(f"runtime evidence rejected: {error}")
    print("runtime evidence accepted")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
