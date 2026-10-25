#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""A read-only or inaccessible Windows page faults on access although its host
page is larger and its neighbours stay writable, and the neighbours work
without faults the program sees, in 64-bit and WoW64 processes
(wine/patches/0974).

Needs the host Wine from tools/build_host_wine.sh (PROSPERO_HOST_WINE, default
.deps/wine-host/install/usr/bin/wine) and the mingw compilers; without them
the check is skipped with a message. Each program runs twice: with the PC's
4 KiB host pages, and with WINE_HOST_PAGE_SIZE=16384, which makes the PC's
Wine keep 16 KiB host pages as the console's does."""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tests" / "wine_subpage_protect.c"
COMPILERS = ("x86_64-w64-mingw32-gcc", "i686-w64-mingw32-gcc")


def main() -> int:
    wine = os.environ.get("PROSPERO_HOST_WINE", str(ROOT / ".deps/wine-host/install/usr/bin/wine"))
    compilers = [shutil.which(name) for name in COMPILERS]
    if not os.access(wine, os.X_OK) or not all(compilers):
        print("wine subpage protect skipped: needs the host Wine and the mingw compilers")
        return 0
    with tempfile.TemporaryDirectory() as directory:
        env = dict(os.environ, WINEPREFIX=str(Path(directory) / "pfx"), WINEDEBUG="-all",
                   WINEDLLOVERRIDES="winemenubuilder.exe=d")
        for compiler in compilers:
            exe = Path(directory) / f"subpage-{Path(compiler).name.split('-')[0]}.exe"
            subprocess.run([compiler, "-O1", "-o", str(exe), str(SOURCE)], check=True)
            for host_page in ("", "16384"):
                run_env = dict(env, WINE_HOST_PAGE_SIZE=host_page) if host_page else env
                done = subprocess.run([wine, str(exe)], capture_output=True, text=True, env=run_env)
                label = f"{exe.name} host pages {host_page or 'native'}"
                assert done.returncode == 0, f"{label}: {done.stdout}{done.stderr}"
                assert "subpage verdict=pass" in done.stdout, f"{label}: {done.stdout}"
    print("wine subpage protect passed: protected pages fault, their neighbours do not")
    return 0


if __name__ == "__main__":
    sys.exit(main())
