#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Fiber and thread stacks commit what Windows commits and grow through their
guard page, where host pages are larger than Windows pages
(wine/patches/0604): 3000 fibers with an exe's default 1 MiB reserve and
4 KiB commit (1000 in a WoW64 process) commit one 16 KiB host page each, a
fiber and a thread recurse 600 KiB deep through their guard pages, and an
overflow still raises STATUS_STACK_OVERFLOW.

Needs the host Wine from tools/build_host_wine.sh (PROSPERO_HOST_WINE, default
.deps/wine-host/install/usr/bin/wine) and the mingw compilers; without them
the check is skipped with a message. Each program runs twice: with the PC's
4 KiB host pages, where Wine commits stacks whole as before and only the
growth and overflow are checked, and with WINE_HOST_PAGE_SIZE=16384, which
makes the PC's Wine keep 16 KiB host pages as the console's does."""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tests" / "wine_fiber_stacks.c"
COMPILERS = (("x86_64-w64-mingw32-gcc", 3000), ("i686-w64-mingw32-gcc", 1000))
HOST_PAGE = 16384


def numbers(output: str, label: str) -> dict[str, int]:
    line = next((line for line in output.splitlines() if line.startswith(label)), "")
    return {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", line)}


def main() -> int:
    wine = os.environ.get("PROSPERO_HOST_WINE", str(ROOT / ".deps/wine-host/install/usr/bin/wine"))
    compilers = [(shutil.which(name), count) for name, count in COMPILERS]
    if not os.access(wine, os.X_OK) or not all(compiler for compiler, _ in compilers):
        print("wine fiber stacks skipped: needs the host Wine and the mingw compilers")
        return 0
    results = []
    with tempfile.TemporaryDirectory() as directory:
        env = dict(os.environ, WINEPREFIX=str(Path(directory) / "pfx"), WINEDEBUG="-all",
                   WINEDLLOVERRIDES="winemenubuilder.exe=d")
        for compiler, count in compilers:
            arch = Path(compiler).name.split("-")[0]
            exe = Path(directory) / f"fiber-stacks-{arch}.exe"
            # The game's exe header: 1 MiB reserved, 4 KiB committed.
            subprocess.run([compiler, "-O1", "-o", str(exe), str(SOURCE),
                            "-Xlinker", "--stack", "-Xlinker", "1048576,4096"], check=True)
            for host_page in (0, HOST_PAGE):
                run_env = dict(env, WINE_HOST_PAGE_SIZE=str(host_page)) if host_page else env
                done = subprocess.run([wine, str(exe), str(count)], capture_output=True, text=True,
                                      env=run_env, timeout=300)
                label = f"{exe.name} host pages {host_page or 'native'}"
                assert done.returncode == 0, f"{label}: {done.stdout}{done.stderr}"
                assert "fiber-stacks verdict=pass" in done.stdout, f"{label}: {done.stdout}"
                fibers = numbers(done.stdout, "fibers=")
                assert fibers.get("fibers") == count, f"{label}: {done.stdout}"
                if host_page:
                    fiber_growth = numbers(done.stdout, "fiber-growth")
                    thread_growth = numbers(done.stdout, "thread-growth")
                    # One host page at the top, and a guard host page under it.
                    assert fibers["committed_per_fiber"] == host_page, f"{label}: {done.stdout}"
                    assert fibers["guard_per_fiber"] == host_page, f"{label}: {done.stdout}"
                    assert fiber_growth["before"] == host_page, f"{label}: {done.stdout}"
                    # A thread has run its start-up (DLL thread attach) by then.
                    assert thread_growth["before"] <= 8 * host_page, f"{label}: {done.stdout}"
                    for growth in (fiber_growth, thread_growth):
                        assert growth["limit_moved"] >= 512 * 1024, f"{label}: {done.stdout}"
                results.append(f"{arch} {host_page or 'native'}: {fibers['committed_per_fiber']} bytes"
                               f" per fiber")
    print("wine fiber stacks passed: stacks grow through their guard page and overflow; committed "
          + ", ".join(results))
    return 0


if __name__ == "__main__":
    sys.exit(main())
