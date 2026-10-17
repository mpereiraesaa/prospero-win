#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""A tree whose subdirectories are all open at once can be listed: 1500
queued directory handles, opened relative to their parents as .NET's
FileSystemEnumerator does (wine/patches/0871).

Needs the host Wine from tools/build_host_wine.sh (PROSPERO_HOST_WINE, default
.deps/wine-host/install/usr/bin/wine) and x86_64-w64-mingw32-gcc; without either
the check is skipped with a message. The PC passes without the patch too: the
same program is the console's check. PROSPERO_TEST_NOFILE=<n> runs Wine and its
server under that descriptor limit; with 512, a server built without
PS5_PARK_DIRECTORIES fails with STATUS_TOO_MANY_OPENED_FILES and one built with
it (-DPS5_PARK_DIRECTORIES) passes."""
from __future__ import annotations

import os
import resource
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tests" / "wine_directory_handles.c"


def main() -> int:
    wine = os.environ.get("PROSPERO_HOST_WINE", str(ROOT / ".deps/wine-host/install/usr/bin/wine"))
    compiler = shutil.which("x86_64-w64-mingw32-gcc")
    if not os.access(wine, os.X_OK) or not compiler:
        print("wine directory handles skipped: needs the host Wine and x86_64-w64-mingw32-gcc")
        return 0
    limit = int(os.environ.get("PROSPERO_TEST_NOFILE", "0"))

    def lower_limit():
        if limit:
            resource.setrlimit(resource.RLIMIT_NOFILE, (limit, limit))

    with tempfile.TemporaryDirectory() as directory:
        exe = Path(directory) / "wine_directory_handles.exe"
        subprocess.run([compiler, "-O1", "-o", str(exe), str(SOURCE), "-lntdll"], check=True)
        env = dict(os.environ, WINEPREFIX=str(Path(directory) / "pfx"), WINEDEBUG="-all",
                   WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d")
        done = subprocess.run([wine, str(exe)], capture_output=True, text=True, env=env,
                              preexec_fn=lower_limit)
        assert done.returncode == 0, f"{done.stdout}{done.stderr}"
        assert "directory-handles verdict=pass" in done.stdout, done.stdout
    print("wine directory handles passed: 3001 directories listed with every subdirectory handle queued"
          + (f" (descriptor limit {limit})" if limit else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
