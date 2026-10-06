#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Pages decommitted and committed again read as zeroes while their
committed neighbours keep their contents, in 64-bit and WoW64 processes
(wine/patches/0893).

Needs the host Wine from tools/build_host_wine.sh (PROSPERO_HOST_WINE, default
.deps/wine-host/install/usr/bin/wine) and the mingw compilers; without them
the check is skipped with a message. The PC's 4 KiB host pages pass without
the patch too: the same programs are the console's check."""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tests" / "wine_decommit_zero.c"
COMPILERS = ("x86_64-w64-mingw32-gcc", "i686-w64-mingw32-gcc")


def main() -> int:
    wine = os.environ.get("PROSPERO_HOST_WINE", str(ROOT / ".deps/wine-host/install/usr/bin/wine"))
    compilers = [shutil.which(name) for name in COMPILERS]
    if not os.access(wine, os.X_OK) or not all(compilers):
        print("wine decommit zero skipped: needs the host Wine and the mingw compilers")
        return 0
    with tempfile.TemporaryDirectory() as directory:
        env = dict(os.environ, WINEPREFIX=str(Path(directory) / "pfx"), WINEDEBUG="-all",
                   WINEDLLOVERRIDES="winemenubuilder.exe=d")
        for compiler in compilers:
            exe = Path(directory) / f"decommit-{Path(compiler).name.split('-')[0]}.exe"
            subprocess.run([compiler, "-O1", "-o", str(exe), str(SOURCE)], check=True)
            done = subprocess.run([wine, str(exe)], capture_output=True, text=True, env=env)
            assert done.returncode == 0, f"{exe.name}: {done.stdout}{done.stderr}"
            assert "decommit verdict=pass" in done.stdout, done.stdout
    print("wine decommit zero passed: recommitted pages read as zeroes, neighbours kept")
    return 0


if __name__ == "__main__":
    sys.exit(main())
