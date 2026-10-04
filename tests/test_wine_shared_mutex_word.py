#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Compile/run the exact atomic header and added server helper bodies.

Native callbacks model legacy list/refcount operations; this does not run
Wine or prove full integration, signal, termination or exception semantics.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "wine/patches/0810-server-ps5-shared-mutex-word.patch"


def added_header():
    text = PATCH.read_text()
    diffs = text.split("diff --git ")[1:]
    match = [part for part in diffs if part.splitlines()[0].endswith(" b/include/wine/ps5_mutex_word.h")]
    assert len(match) == 1
    return "\n".join(line[1:] for line in match[0].splitlines()
                     if line.startswith("+") and not line.startswith("+++")) + "\n"


def added_server_helpers():
    parts = PATCH.read_text().split("diff --git ")[1:]
    part = next(p for p in parts if p.splitlines()[0].endswith(" b/server/mutex.c"))
    additions = "\n".join(line[1:] for line in part.splitlines()
                          if line.startswith("+") and not line.startswith("+++"))
    core = additions[additions.index("/* Native-only cold backend."):]
    core = core[:core.index("\n#endif")]
    cold = additions[additions.index("void ps5_mutex_retire_object("):]
    cold = cold[:cold.index("\n#endif")]
    return core + "\n" + cold + "\n"


def main():
    with tempfile.TemporaryDirectory(prefix="pw-shared-mutex-word-") as temp:
        folder = Path(temp)
        (folder / "ps5_mutex_word.h").write_text(added_header())
        (folder / "ps5_mutex_server.inc").write_text(added_server_helpers())
        binary = folder / "test"
        command = shlex.split(os.environ.get("CC", "cc"))
        command += shlex.split(os.environ.get("CFLAGS", "-O2 -g -Wall -Wextra -Werror"))
        command += ["-std=gnu11", "-pthread", "-I", str(folder),
                    str(ROOT / "tests/test_wine_shared_mutex_word.c"), "-o", str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True, timeout=60)
        server_command = command.copy()
        server_command[server_command.index(str(ROOT / "tests/test_wine_shared_mutex_word.c"))] = str(
            ROOT / "tests/test_wine_shared_mutex_server.c")
        subprocess.run(server_command, check=True)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
