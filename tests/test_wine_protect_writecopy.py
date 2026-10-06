#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""VirtualProtect on a written page of an image's data reports the old
protection as PAGE_READWRITE, as Windows does (wine/patches/0891).

Needs the host Wine from tools/build_host_wine.sh (PROSPERO_HOST_WINE, default
.deps/wine-host/install/usr/bin/wine) and i686-w64-mingw32-gcc; without either
the check is skipped with a message, since the host gate runs without them."""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tests" / "wine_protect_writecopy.c"


def main() -> int:
    wine = os.environ.get("PROSPERO_HOST_WINE", str(ROOT / ".deps/wine-host/install/usr/bin/wine"))
    compiler = shutil.which("i686-w64-mingw32-gcc")
    if not os.access(wine, os.X_OK) or not compiler:
        print("wine protect write-copy skipped: needs the host Wine and i686-w64-mingw32-gcc")
        return 0
    with tempfile.TemporaryDirectory() as directory:
        exe = Path(directory) / "protect.exe"
        subprocess.run([compiler, "-O1", "-o", str(exe), str(SOURCE)], check=True)
        env = dict(os.environ, WINEPREFIX=str(Path(directory) / "pfx"), WINEDEBUG="-all",
                   WINEDLLOVERRIDES="winemenubuilder.exe=d")
        done = subprocess.run([wine, str(exe)], capture_output=True, text=True, env=env)
        assert done.returncode == 0, f"{done.stdout}{done.stderr}"
        assert "old=0x4" in done.stdout, done.stdout
    print("wine protect write-copy passed: a written image page reports PAGE_READWRITE")
    return 0


if __name__ == "__main__":
    sys.exit(main())
