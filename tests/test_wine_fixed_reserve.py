#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Memory reserved at a chosen address outside Wine's reserved areas commits and takes
writes, also after it was released and reserved again (wine/patches/0897).

Needs the host Wine from tools/build_host_wine.sh (PROSPERO_HOST_WINE, default
.deps/wine-host/install/usr/bin/wine) and x86_64-w64-mingw32-gcc; without either
the check is skipped with a message. The PC passes without the patch too: the
same program is the console's check."""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tests" / "wine_fixed_reserve.c"


def main() -> int:
    wine = os.environ.get("PROSPERO_HOST_WINE", str(ROOT / ".deps/wine-host/install/usr/bin/wine"))
    compiler = shutil.which("x86_64-w64-mingw32-gcc")
    if not os.access(wine, os.X_OK) or not compiler:
        print("wine fixed reserve skipped: needs the host Wine and x86_64-w64-mingw32-gcc")
        return 0
    with tempfile.TemporaryDirectory() as directory:
        exe = Path(directory) / "wine_fixed_reserve.exe"
        subprocess.run([compiler, "-O1", "-o", str(exe), str(SOURCE)], check=True)
        env = dict(os.environ, WINEPREFIX=str(Path(directory) / "pfx"), WINEDEBUG="-all",
                   WINEDLLOVERRIDES="winemenubuilder.exe=d")
        done = subprocess.run([wine, str(exe)], capture_output=True, text=True, env=env)
        assert done.returncode == 0, f"{done.stdout}{done.stderr}"
        assert "fixed-reserve verdict=pass" in done.stdout, done.stdout
    print("wine fixed reserve passed: memory reserved at a chosen address commits and takes writes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
