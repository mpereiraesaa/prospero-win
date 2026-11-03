#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tests/wine_mapvk.c (32-bit, as the game that found it): MapVirtualKeyExA
gives 0 for every code 0x100-0x2ff under MAPVK_VSC_TO_VK and
MAPVK_VSC_TO_VK_EX, while plain and E0 scan codes still translate
(wine/patches/0904).

Needs the host Wine from tools/build_host_wine.sh (PROSPERO_HOST_WINE, default
.deps/wine-host/install/usr/bin/wine) and i686-w64-mingw32-gcc; without either
the check is skipped with a message. It runs headless (no DISPLAY), so win32u
has the null user driver and answers from its generic keyboard tables, the
path the PS5 driver takes. With PW_TEST_WINEX11=1 and DISPLAY set it runs a
second time with the display, through winex11's keymap; it opens no window. A
win32u without the patch gives verdict=fail on the null driver: 39 of the 512
codes translate in each mode, the first 0x110 to VK_MEDIA_PREV_TRACK."""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tests" / "wine_mapvk.c"


def run(wine: str, exe: Path, prefix: Path, display: bool) -> str:
    env = dict(os.environ, WINEPREFIX=str(prefix), WINEDEBUG="-all",
               WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d")
    if not display:
        for name in ("DISPLAY", "WAYLAND_DISPLAY"):
            env.pop(name, None)
    done = subprocess.run([wine, str(exe)], capture_output=True, text=True, env=env, timeout=180)
    subprocess.run([wine + "server", "-k"], capture_output=True, env=env)
    lines = [line.strip() for line in done.stdout.splitlines() if line.startswith("MAPVK ")]
    assert len(lines) == 1, f"no single MAPVK line:\n{done.stdout}{done.stderr[-3000:]}"
    assert lines[0].startswith("MAPVK verdict=pass "), lines[0]
    return lines[0]


def main() -> int:
    wine = os.environ.get("PROSPERO_HOST_WINE", str(ROOT / ".deps/wine-host/install/usr/bin/wine"))
    compiler = shutil.which("i686-w64-mingw32-gcc")
    if not os.access(wine, os.X_OK) or not compiler:
        print("wine mapvk skipped: needs the host Wine and i686-w64-mingw32-gcc")
        return 0

    with tempfile.TemporaryDirectory() as directory:
        exe = Path(directory) / "wine_mapvk.exe"
        subprocess.run([compiler, "-O1", "-Wall", "-Wextra", "-Werror", "-o", str(exe), str(SOURCE), "-luser32"],
                       check=True)
        line = run(wine, exe, Path(directory) / "pfx", False)
        print(f"wine mapvk passed (null driver): {line}")
        if os.environ.get("PW_TEST_WINEX11") == "1" and os.environ.get("DISPLAY"):
            line = run(wine, exe, Path(directory) / "pfx-x11", True)
            print(f"wine mapvk passed (display): {line}")
        else:
            print("wine mapvk display run skipped: needs PW_TEST_WINEX11=1 and DISPLAY")
    return 0


if __name__ == "__main__":
    sys.exit(main())
