#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""A 32-bit program's DIB section over its own section has writable bits
(wine/patches/0892): the console placed them above 4 GiB.

Needs the host Wine from tools/build_host_wine.sh (PROSPERO_HOST_WINE, default
.deps/wine-host/install/usr/bin/wine) and i686-w64-mingw32-gcc; without either
the check is skipped with a message. On Linux the bits were already low, so
this documents the contract rather than failing before the patch."""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tests" / "wine_dib_section.c"


def main() -> int:
    wine = os.environ.get("PROSPERO_HOST_WINE", str(ROOT / ".deps/wine-host/install/usr/bin/wine"))
    compiler = shutil.which("i686-w64-mingw32-gcc")
    if not os.access(wine, os.X_OK) or not compiler:
        print("wine DIB section skipped: needs the host Wine and i686-w64-mingw32-gcc")
        return 0
    with tempfile.TemporaryDirectory() as directory:
        exe = Path(directory) / "dib.exe"
        subprocess.run([compiler, "-O1", "-o", str(exe), str(SOURCE), "-lgdi32"], check=True)
        env = dict(os.environ, WINEPREFIX=str(Path(directory) / "pfx"), WINEDEBUG="-all",
                   WINEDLLOVERRIDES="winemenubuilder.exe=d")
        done = subprocess.run([wine, str(exe)], capture_output=True, text=True, env=env)
        assert done.returncode == 0 and "written" in done.stdout, f"{done.stdout}{done.stderr}"
    print("wine DIB section passed: a section-backed DIB's bits are writable by the 32-bit program")
    return 0


if __name__ == "__main__":
    sys.exit(main())
