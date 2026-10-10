#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tools/native_profile_frames.py on a synthetic log: the frame lengths, the
spike and normal sets, the CS thread's bucket split in each, the RADV
entry points joined to it by TSC, the events, DXVK's commands, the CSV,
and the refusals without a clock or presents."""

import io
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import native_profile_frames as npf  # noqa: E402

HZ = 1_000_000_000  # 1 tick = 1 ns
MS = HZ // 1000


def present(frame, tsc, interval):
    return f"12\t34\tINFO\tWINE PW_VK_RADV_PRESENT version=1 frame={frame} thread=2 tsc={tsc} interval={interval} ticks=500 result=0"


def native(frame, tid, tsc, wall, unix, syscall, other=0, fs=0, unix_calls=90, frames=1):
    return (f"12\t34\tINFO\tWINE PW_NATIVE_PROFILE version=3 frame={frame} frames={frames} tid={tid:04x} tsc={tsc} "
            f"tsc_hz={HZ} wall={wall} unix={unix} syscall={syscall} other={other} fs={fs} unix_calls={unix_calls} "
            f"syscall_calls=20")


def radv(frame, thread, tsc, entries, frames=1):
    body = " ".join(f"{name}={calls}/{ticks}" for name, calls, ticks in entries)
    calls = sum(e[1] for e in entries)
    ticks = sum(e[2] for e in entries)
    return (f"12\t34\tINFO\tWINE PW_VK_RADV_PROFILE version=1 frame={frame} frames={frames} thread={thread} "
            f"tsc={tsc} wall=1000 calls={calls} ticks={ticks} {body}")


def build_log():
    lines = ["12\t34\tINFO\tWINE PW_VK_RADV_PROFILE version=1 start tsc_hz=%d event_us=200 functions=173 enabled_by=env" % HZ,
             "PW_NATIVE_PROFILE version=2 tid=004c teb=1 tsc=5 tsc_hz=%d host_calls=1" % HZ]
    tsc = 0
    for frame in range(1, 41):
        spike = frame % 10 == 0
        interval = 40 * MS if spike else 16 * MS
        tsc += interval
        lines.append(present(frame, tsc, interval))
        # CS thread 0x4c: guest grows in spikes; another thread 0x24 with few Unix calls.
        if spike:
            lines.append(native(frame, 0x4c, tsc - 10, 40 * MS, 8 * MS, 2 * MS, fs=1 * MS, unix_calls=100))
            lines.append(radv(frame, 1, tsc - 10, [("vkCmdDraw", 400, 6 * MS), ("vkCreateGraphicsPipelines", 1, 2 * MS)]))
        else:
            lines.append(native(frame, 0x4c, tsc - 10, 16 * MS, 4 * MS, 2 * MS, fs=1 * MS, unix_calls=90))
            lines.append(radv(frame, 1, tsc - 10, [("vkCmdDraw", 300, 3 * MS), ("vkCmdBindPipeline", 50, 1 * MS)]))
        lines.append(native(frame, 0x24, tsc - 5, 16 * MS, 1 * MS, 14 * MS, unix_calls=3))
        lines.append(radv(frame, 2, tsc, [("vkQueueSubmit2", 16, 1 * MS), ("vkQueuePresentKHR", 1, 500)]))
        # DXVK's frame id runs 3 behind the driver's count here; its line comes before the present.
        lines.append(f"12\t34\tINFO\tWINE DXVK_CS_PROFILE version=1 frame={frame - 3} frames=1 tsc={tsc - 100} wall={interval} "
                     f"busy={(30 if spike else 12) * MS} chunks=70 cmds=9000 c3=1000/{(20 if spike else 8) * MS} c7=10/{1 * MS}")
    lines.append("12\t34\tINFO\tWINE PW_VK_RADV_EVENT version=1 frame=10 thread=1 tsc=400000000 fn=vkCreateGraphicsPipelines ticks=2000000 count=1 flags=0x800 library=0 link_time_opt=1 fail_on_compile=0")
    lines.append("12\t34\tINFO\tWINE PW_VK_RADV_EVENT version=1 frame=3 thread=2 tsc=50000000 fn=vkQueueSubmit2 ticks=300000 submits=1")
    lines.append("12\t34\tINFO\tWINE DXVK_CS_PROFILE version=1 start tsc_hz=%d" % HZ)
    lines.append("12\t34\tINFO\tWINE DXVK_CS_PROFILE name id=3 text=dxvk::DxvkContext::draw")
    lines.append("12\t34\tINFO\tWINE DXVK_CS_PROFILE name id=7 text=dxvk::DxvkContext::bindShader")
    # A line spanning two frames is left out of the per-frame means.
    lines.append(native(41, 0x4c, tsc + 1000, 32 * MS, 8 * MS, 4 * MS, frames=2))
    return lines


def main() -> int:
    lines = build_log()
    log = npf.parse(lines)
    assert log.tsc_hz == HZ and log.event_us == 200
    assert len(log.presents) == 40 and log.presents[10]["interval"] == 40 * MS
    lengths = npf.frame_ms(log, HZ)
    assert abs(lengths[10] - 40) < 1e-9 and abs(lengths[1] - 16) < 1e-9
    normal, spikes = npf.classify(lengths, 25, 20)
    assert spikes == {10, 20, 30, 40} and len(normal) == 36
    assert npf.cs_tid(log) == 0x4c
    split = npf.native_split(log.native[0x4c], spikes, HZ)
    assert split["frames"] == 4 and abs(split["wall"] - 40) < 1e-9 and abs(split["unix"] - 8) < 1e-9
    assert abs(split["guest"] - 29) < 1e-9 and abs(split["unix_calls"] - 100) < 1e-9, split
    base = npf.native_split(log.native[0x4c], normal, HZ)
    assert abs(base["guest"] - 9) < 1e-9 and base["frames"] == 36
    assert npf.native_split(log.native[0x4c], set(), HZ) == {"frames": 0}
    assert npf.radv_thread_for(log, 0x4c) == 1 and npf.radv_thread_for(log, 0x24) is None
    fns = npf.per_function(log.radv[1], spikes, HZ)
    assert abs(fns["vkCmdDraw"][1] - 6) < 1e-9 and abs(fns["vkCmdDraw"][0] - 400) < 1e-9
    assert abs(fns["vkCreateGraphicsPipelines"][1] - 2) < 1e-9 and "vkCmdBindPipeline" not in fns
    assert len(log.events) == 2 and log.events[0][1] == "vkCreateGraphicsPipelines"
    assert log.events[0][2].startswith("count=1 flags=0x800") and log.events[1][2] == "submits=1"
    assert log.dxvk_names == {3: "dxvk::DxvkContext::draw", 7: "dxvk::DxvkContext::bindShader"}
    assert len(log.dxvk) == 40 and log.dxvk[9][1][3] == (1000, 20 * MS)
    assert npf.percentile([1, 2, 3, 4, 5], 0.5) == 3 and npf.percentile([], 0.5) == 0

    out = io.StringIO()
    assert npf.report(log, 25, 20, None, 15, out) == 0
    text = out.getvalue()
    assert "frames: 40" in text and "spikes (>= 25 ms): 4" in text and "normal (<= 20 ms): 36" in text
    assert "CS thread tid=004c" in text and "(1 spanning several frames, left out)" in text
    assert "guest" in text and "+20.00" in text          # the guest bucket grows by 20 ms in spikes
    assert "vkCmdDraw" in text and "vkCreateGraphicsPipelines" in text and "RADV thread 2" in text
    assert "events over 200 us: 2 total, 1 in spike frames (waits left out: 0)" in text
    aligned = npf.align_dxvk(log)
    assert [f["frame"] for f, _ in aligned] == list(range(1, 41)) and npf.align_dxvk(npf.Log()) == []
    assert "dxvk::DxvkContext::draw" in text and "DXVK CS thread: 40 frame lines, 36 normal, 4 spike" in text
    pretty = ("const char* dxvk::DxvkCsTypedCmd<T>::profileName() const [with T = dxvk::D3D9DeviceEx::"
              "UpdatePushConstant<24, 4>(const void*)::<lambda(dxvk::DxvkContext*)>]")
    assert npf.short_dxvk_name(pretty) == "D3D9DeviceEx::UpdatePushConstant<24, 4>"
    assert npf.short_dxvk_name("const char* dxvk::DxvkCsDataCmd<T, M>::profileName() const [with T = dxvk::D3D9DeviceEx::"
                               "Clear(DWORD)::<lambda(uint32_t)>; M = int]") == "D3D9DeviceEx::Clear"
    assert npf.short_dxvk_name("plain") == "plain"
    cut = npf.window(log, HZ, 0.3, 0.5)     # presents 19..31 of 16 ms, plus the spikes' extra
    assert min(cut.presents) > 1 and max(cut.presents) < 40 and all(f["frame"] in cut.presents for f, _ in cut.radv[1])
    assert npf.window(log, HZ, None, None) is log
    assert "busy ms: normal 12.00  spike 30.00" in text
    assert "worst frames: #10 40.0 ms" in text
    # --tid picks another thread; a thread without RADV lines says so.
    out = io.StringIO()
    assert npf.report(log, 25, 20, 0x24, 15, out) == 0
    assert "CS thread tid=0024" in out.getvalue() and "no RADV lines joined" in out.getvalue()
    # Without a clock or presents the tool refuses.
    out = io.StringIO()
    assert npf.report(npf.Log(), 25, 20, None, 15, out) == 1 and "no TSC rate" in out.getvalue()
    out = io.StringIO()
    only_clock = npf.parse(lines[:1])
    assert npf.report(only_clock, 25, 20, None, 15, out) == 1 and "no PW_VK_RADV_PRESENT" in out.getvalue()

    with tempfile.TemporaryDirectory() as directory:
        log_path = Path(directory) / "session.log"
        log_path.write_text("\n".join(lines) + "\n")
        csv_path = Path(directory) / "frames.csv"
        run = subprocess.run([sys.executable, str(ROOT / "tools/native_profile_frames.py"), str(log_path),
                              "--csv", str(csv_path), "--top", "5"], capture_output=True, text=True)
        assert run.returncode == 0 and "CS thread tid=004c" in run.stdout, run
        rows = csv_path.read_text().splitlines()
        assert rows[0].startswith("frame,length_ms,wall_ms,guest_ms") and len(rows) == 41
        assert rows[10].startswith("10,40.000,40.000,29.000,8.000,2.000,0.000,1.000,100,8.000,401,30.000,9000"), rows[10]
        run = subprocess.run([sys.executable, str(ROOT / "tools/native_profile_frames.py"), str(log_path),
                              "--tsc-hz", str(2 * HZ)], capture_output=True, text=True)
        assert run.returncode == 0 and "p50 8.0" in run.stdout, run.stdout
    print("native_profile_frames: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
