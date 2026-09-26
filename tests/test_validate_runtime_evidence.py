# SPDX-License-Identifier: LGPL-2.1-or-later
from __future__ import annotations
import importlib.util
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("validator", ROOT / "tools/validate_runtime_evidence.py")
MODULE = importlib.util.module_from_spec(SPEC); assert SPEC and SPEC.loader; SPEC.loader.exec_module(MODULE)


def transcript(end: bool = False) -> str:
    lines = ["HELLO ps5log/1 title=PPSA99995 app=prospero-win boot=0x1 tag=test",
             "1\t1000000000\tINFO\tPW_RUNTIME_BEGIN schema=1",
             "2\t1100000000\tINFO\tPW_STATE_LOAD status=ok bytes=473",
             "3\t1200000000\tINFO\tPW_PAD_OPEN handle=7 read=scePadRead",
             "4\t1300000000\tINFO\tPW_RUNTIME_READY imports=207",
             "5\t3000000000\tINFO\tPW_RUNTIME_HEARTBEAT retired=10 calls=2 flips=3 audio_blocks=4 pad_samples=5 pad_connected=5 pad_events=5 pad_read_errors=0 profile_lookups=2 profile_missing=0 profile_errors=0 profile_bytes=10 idle_yields=1"]
    if end:
        lines += ["6\t3050000000\tINFO\tPW_PAD_QUIT source=create action=WM_QUIT",
                  "7\t3100000000\tINFO\tPW_RUNTIME_TEARDOWN schema=2 state=ok pad=ok audio=ok gdi=ok video=ok agc=ok dbt=ok image=ok stack=ok thread=ok crt=ok heap=ok session=1 outcome=1 supervisor_state=0 supervisor_cleanup=ok",
                  "8\t3200000000\tINFO\tPW_RUNTIME_END reason=validation-deadline flips=5 audio_blocks=6",
                  "BYE seq=8 reason=validation-deadline"]
    return "\n".join(lines) + "\n"


def async_transcript() -> str:
    return transcript().replace(
        "PW_RUNTIME_HEARTBEAT ",
        "PW_AUDIO_QUEUE worker=1 enqueues=3 completions=2 depth=1 high_water=8 full=0 blocks=9 output_errors=0\n"
        "6\t3100000000\tINFO\tPW_RUNTIME_HEARTBEAT schema=2 ",
    ).replace(
        "retired=10 ",
        "retired=10 dbt_dispatches=9 dbt_compiles=2 dbt_hits=7 dbt_misses=2 "
        "dbt_lookup_probes=11 dbt_max_probe=2 dbt_protect_calls=4 dbt_protect_bytes=65536 "
        "audio_enqueues=3 audio_completions=2 audio_queue_full=0 audio_errors=0 "
        "loop_gap_max_ns=1000000 ",
    )


def check(text: str, continuous: bool, accepted: bool) -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "run.log"; path.write_text(text)
        try:
            MODULE.validate(path, continuous, 1, 2, 2)
        except ValueError:
            assert not accepted
        else:
            assert accepted


def vk_transcript() -> str:
    text = transcript(True).replace(
        "4\t1300000000\tINFO\tPW_RUNTIME_READY",
        "4\t1250000000\tINFO\tPW_PRESENT_OPEN schema=1 backend=vk-wsi status=ok failed_call=none\n"
        "4\t1300000000\tINFO\tPW_RUNTIME_READY")
    frames = ("PW_VIDEO_FRAME flips=1 backend=vk-wsi slot=0 token=1\n"
              "5\t2000000000\tINFO\tPW_VIDEO_FRAME flips=2 backend=vk-wsi slot=1 token=2\n"
              "6\t3000000000\tINFO\tPW_RUNTIME_HEARTBEAT ")
    text = text.replace("PW_RUNTIME_HEARTBEAT ", frames)
    text = text.replace("heap=ok ", "heap=ok present_backend=vk-wsi present_flips=5 present_failed_call=none ")
    return renumber(text)


def renumber(text: str) -> str:
    lines, seq = [], 0
    for line in text.splitlines():
        if line[:1].isdigit():
            seq += 1; line = f"{seq}\t" + line.split("\t", 1)[1]
        elif line.startswith("BYE"):
            line = f"BYE seq={seq} " + line.split(" ", 2)[2]
        lines.append(line)
    return "\n".join(lines) + "\n"


def check_vk(text: str, accepted: bool) -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "vk.log"; path.write_text(text)
        try:
            MODULE.validate(path, False, 1, 2, 2, present_backend="vk-wsi")
        except ValueError:
            assert not accepted
        else:
            assert accepted


TEARDOWN_OK = ("state=ok pad=ok audio=ok gdi=ok video=ok agc=ok dbt=ok image=ok stack=ok "
               "thread=ok crt=ok heap=ok supervisor_cleanup=ok devices=retained")


def launcher_transcript(cycles: int = 2) -> str:
    body = ["PW_RUNTIME_BEGIN schema=1 launcher=1",
            "PW_LAUNCHER_OPEN schema=1 profiles=2 listed=2 backend=vk-wsi display_opens=1 input=script"]
    for session in range(1, cycles + 1):
        body += [f"PW_LAUNCHER_LAUNCH schema=1 session={session} id=pinball backend=vk-wsi display_opens=1",
                 "PW_PAD_QUIT schema=1 source=combo action=WM_QUIT held_ms=1013",
                 f"PW_RUNTIME_TEARDOWN schema=2 reason=crt-exit {TEARDOWN_OK}",
                 f"PW_SESSION_END schema=1 session={session} reason=crt-exit cleanup=ok flips=90 audio_blocks=900",
                 f"PW_LAUNCHER_RETURN schema=1 session={session} status=complete cleanup=ok backend=vk-wsi display_opens=1"]
    body += [f"PW_LAUNCHER_TEARDOWN schema=1 reason=validation-deadline sessions={cycles} returns={cycles} "
             "pad=ok video=ok agc=ok display_opens=1",
             "PW_RUNTIME_END schema=1 reason=validation-deadline"]
    lines = ["HELLO ps5log/1 title=PPSA99995 app=prospero-win boot=0x1 tag=test"]
    lines += [f"{i}\t{1000000000 + i * 1000000000}\tINFO\t{record}" for i, record in enumerate(body, 1)]
    lines.append(f"BYE seq={len(body)} reason=validation-deadline")
    return "\n".join(lines) + "\n"


def check_launcher(text: str, accepted: bool, cycles: int = 2) -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "launcher.log"; path.write_text(text)
        try:
            MODULE.validate_launcher(path, cycles, 1)
        except ValueError:
            assert not accepted
        else:
            assert accepted


def main() -> int:
    check_launcher(launcher_transcript(), True)
    check_launcher(launcher_transcript(3), True, 3)
    check_launcher(launcher_transcript(1), False)
    check_launcher(transcript(True), False)
    check_launcher(launcher_transcript().replace("display_opens=1 input", "display_opens=2 input"), False)
    check_launcher(launcher_transcript().replace(
        "session=2 id=pinball backend=vk-wsi display_opens=1", "session=2 id=pinball backend=vk-wsi display_opens=2"), False)
    check_launcher(launcher_transcript().replace("session=2 id=pinball backend=vk-wsi",
                                                 "session=2 id=pinball backend=agc-dma"), False)
    check_launcher(launcher_transcript().replace("source=combo", "source=create"), False)
    check_launcher(launcher_transcript().replace("devices=retained", "devices=closed", 1), False)
    check_launcher(launcher_transcript().replace("heap=ok", "heap=state", 1), False)
    check_launcher(launcher_transcript().replace("cleanup=ok flips", "cleanup=state flips", 1), False)
    check_launcher(launcher_transcript().replace("flips=90", "flips=0", 1), False)
    check_launcher(launcher_transcript().replace("status=complete", "status=failed", 1), False)
    check_launcher(launcher_transcript().replace("session=2 id", "session=3 id"), False)
    check_launcher(launcher_transcript().replace("returns=2", "returns=1"), False)
    check_launcher(launcher_transcript().replace("video=ok agc=ok display_opens=1\n", "video=state agc=ok display_opens=1\n"), False)
    text = launcher_transcript()
    check_launcher(text[:text.rindex("BYE ")] + "BYE seq=14 reason=other\n", False)
    check_launcher(text.replace("PW_RUNTIME_BEGIN schema=1 launcher=1", "PW_RUNTIME_ABORT stage=x"), False)
    check_launcher(text.replace("\t4000000000\t", "\t4000000001\t").replace("3\t4000000001", "4\t4000000001"), False)
    check_vk(vk_transcript(), True)
    check_vk(transcript(True), False)
    check_vk(vk_transcript().replace("status=ok failed_call", "status=state failed_call"), False)
    check_vk(vk_transcript().replace("slot=1", "slot=0"), False)
    check_vk(vk_transcript().replace("flips=2 backend=vk-wsi slot=1 token=2",
                                     "flips=2 backend=vk-wsi slot=1 token=1"), False)
    check_vk(vk_transcript().replace("flips=2 backend=vk-wsi", "flips=2 backend=agc-dma"), False)
    check_vk(vk_transcript().replace("present_flips=5", "present_flips=4"), False)
    check_vk(vk_transcript().replace("present_failed_call=none", "present_failed_call=vkQueueSubmit"), False)
    check_vk(renumber(vk_transcript().replace(
        "PW_RUNTIME_READY", "PW_PRESENT_FAIL status=state\n4\t1300000000\tINFO\tPW_RUNTIME_READY")), False)
    check(transcript(), True, True); check(transcript(True), False, True)
    check(async_transcript(), True, True)
    check(async_transcript().replace("audio_errors=0", "audio_errors=1"), True, False)
    check(async_transcript().replace("worker=1", "worker=0"), True, False)
    check(transcript().replace("profile_errors=0", "profile_errors=1"), True, False)
    check(transcript().replace("3\t", "4\t", 1), True, False)
    check(transcript(True).replace("pad=ok", "pad=state"), False, False)
    check(transcript(True).replace("flips=5", "flips=1"), False, False)
    check(transcript(True).replace("supervisor_state=0", "supervisor_state=4"), False, False)
    check(transcript(True).replace("supervisor_cleanup=ok", "supervisor_cleanup=state"), False, False)
    check(transcript(True).replace("session=1 outcome=1", "session=1 outcome=0"), False, False)
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "gameplay.log"; path.write_text(transcript(True))
        MODULE.validate(path, False, 1, 2, 2, 5, True)
        path.write_text(transcript(True).replace("PW_PAD_QUIT", "PW_PAD_OTHER"))
        try:
            MODULE.validate(path, False, 1, 2, 2, 5, True)
        except ValueError:
            pass
        else:
            raise AssertionError("missing physical quit evidence was accepted")
    check(transcript(True).replace("\nBYE", "\nPW_RUNTIME_ABORT bad=1\nBYE"), False, False)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
