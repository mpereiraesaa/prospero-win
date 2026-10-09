#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""check_setup must report a missing unzip, which the host suite shells out to.

A fresh host with only the documented base packages once passed
tools/check_setup.py and then failed make all inside
tests/test_publish_release.py: tools/publish_release.sh shells out to
unzip, and the missing binary surfaced as a misleading "is not a zip"
refusal. This test keeps the prerequisite checker ahead of that path."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CHECK = ROOT / "tools" / "check_setup.py"
SIBLING_COMMANDS = ("git", "make", "cc", "clang", "i686-w64-mingw32-gcc")


def run_with_path(path: str) -> subprocess.CompletedProcess:
    environment = dict(os.environ, PATH=path)
    return subprocess.run([sys.executable, str(CHECK)], capture_output=True,
                          text=True, env=environment)


def main() -> int:
    """A PATH carrying every sibling tool but no unzip must be reported."""
    with tempfile.TemporaryDirectory() as staging:
        stage = Path(staging)
        for command in SIBLING_COMMANDS:
            found = shutil.which(command)
            if found is None:
                print(f"test_check_setup: host lacks {command}; "
                      "run tools/check_setup.py first")
                return 1
            (stage / command).symlink_to(found)
        result = run_with_path(str(stage))
        if result.returncode == 0 or "unzip" not in result.stdout:
            print("test_check_setup: missing unzip was not reported")
            print(result.stdout, result.stderr, sep="", end="")
            return 1
    print("test_check_setup: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
