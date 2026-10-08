#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tools/native_profile_split.py on a synthetic log: per-thread shares,
call rates, the window, threads too short to report, and other lines."""

from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import native_profile_split as nps  # noqa: E402

HZ = 1_000_000_000


def report(tid, tsc, unix=0, sys_=0, other=0, host_fs=0, guest_fs=0, unix_calls=0, sys_calls=0):
    return (f"123\t456\tINFO\tWINE PW_NATIVE_PROFILE version=2 tid={tid:04x} teb=7ffd0000 tsc={tsc} "
            f"tsc_hz={HZ} host_calls=1 guest_calls=1 host_sysarch_ticks={host_fs} "
            f"guest_sysarch_ticks={guest_fs} unix_calls={unix_calls} syscall_calls={sys_calls} "
            f"unix_host_ticks={unix} syscall_host_ticks={sys_} other_host_ticks={other}")


def main() -> int:
    lines = [
        "WINE 0040:trace:fps:x @ approx 60.00fps",
        "1\t2\tINFO\tWINE PW_NATIVE_PROFILE cpu_clock_mhz=3500 cpu=3 tid=0000",
        "1\t2\tINFO\tWINE PW_NATIVE_PROFILE cpu_clock_mhz=1600 cpu=5 tid=004c",
        "PW_NATIVE_PROFILE version=1 teb=1 tsc=5 host_calls=1",       # old format: ignored
        report(0x4c, 0),
        report(0x24, 0),
        report(0x4c, 2 * HZ, unix=HZ // 2, sys_=HZ // 10, other=HZ // 100, host_fs=HZ // 50,
               guest_fs=HZ // 50, unix_calls=2000, sys_calls=100),
        report(0x24, 2 * HZ, sys_=HZ, sys_calls=40),
        report(0x50, 1 * HZ),                                          # one report: no interval
        report(0x4c, 4 * HZ, unix=HZ, sys_=HZ // 5, other=HZ // 50, host_fs=HZ // 25,
               guest_fs=HZ // 25, unix_calls=4000, sys_calls=200),
    ]
    rows = nps.split(nps.parse(lines))
    assert [r["tid"] for r in rows] == [0x4c, 0x24], rows
    cs = rows[0]
    assert abs(cs["seconds"] - 4) < 1e-9
    assert abs(cs["unix"] - 25) < 1e-9 and abs(cs["syscall"] - 5) < 1e-9, cs
    assert abs(cs["other"] - 0.5) < 1e-9 and abs(cs["fs"] - 2) < 1e-9, cs
    assert abs(cs["guest"] - 67.5) < 1e-9, cs
    assert abs(cs["unix_rate"] - 1000) < 1e-9 and abs(cs["syscall_rate"] - 50) < 1e-9, cs
    assert abs(rows[1]["syscall"] - 50) < 1e-9 and rows[1]["unix_rate"] == 0
    # The window keeps only reports inside it: 0x4c from 2 s to 4 s.
    late = nps.split(nps.parse(lines), start=1.5)
    assert [r["tid"] for r in late] == [0x4c] and abs(late[0]["unix"] - 25) < 1e-9, late
    assert nps.split(nps.parse(lines), min_wall=3) == [r for r in rows if r["tid"] == 0x4c]
    assert nps.split([]) == []
    assert nps.clocks(lines) == [(3500, 3, 0), (1600, 5, 0x4c)]
    with tempfile.TemporaryDirectory() as directory:
        log = Path(directory) / "session.log"
        log.write_text("\n".join(lines) + "\n")
        run = subprocess.run([sys.executable, str(ROOT / "tools/native_profile_split.py"), str(log)],
                             capture_output=True, text=True)
        assert run.returncode == 0 and "4c" in run.stdout and "67.5" in run.stdout, run
        assert "2 samples, min 1600 MHz, median 3500 MHz, max 3500 MHz" in run.stdout, run
        empty = Path(directory) / "empty.log"
        empty.write_text("nothing\n")
        run = subprocess.run([sys.executable, str(ROOT / "tools/native_profile_split.py"), str(empty)],
                             capture_output=True, text=True)
        assert run.returncode == 1 and "no PW_NATIVE_PROFILE" in run.stderr, run
    print("native profile split passed: shares, rates, window, short threads, core-clock samples, old format and empty logs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
