#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Compile exact added server helpers with bounded native fixture callbacks."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "wine/patches/0886-server-ps5-shared-sync-lifetime.patch"


def added(text, name):
    part, = [p for p in text.split("diff --git ")[1:]
             if p.splitlines()[0].endswith(" b/" + name)]
    return "\n".join(line[1:] for line in part.splitlines()
                     if line.startswith("+") and not line.startswith("+++")) + "\n"


def block(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)] + "\n"


def main():
    text = PATCH.read_text()
    events = added(text, "server/event.c")
    semaphores = added(text, "server/semaphore.c")
    core = block(events, "/* All metadata/list/callback", "\n#endif")
    queues = block(events, "#define ps5_event_enter_slow", "\n#else")
    queues += block(semaphores, "#define ps5_semaphore_enter_slow", "\n#else")
    cold = block(events, "static unsigned int ps5_event_read_value", "\n#endif")
    cold += block(semaphores, "static unsigned int ps5_semaphore_read_value", "\n#endif")
    header = added(text, "server/ps5_sync.h")
    # Fixture supplies its own list/refcount/legacy callbacks. Keep the actual
    # state metadata and helper bodies; only redirect the external includes.
    header = header.replace('#include "wine/list.h"\n', "")
    header = header.replace('"wine/ps5_sync_word.h"', '"ps5_sync_word.h"')
    word = added((ROOT / "wine/patches/0885-server-ps5-shared-sync-word.patch").read_text(),
                 "include/wine/ps5_sync_word.h")
    with tempfile.TemporaryDirectory(prefix="pw-shared-sync-server-") as temp:
        folder = Path(temp)
        (folder / "ps5_sync_word.h").write_text(word)
        (folder / "ps5_sync.h").write_text(header)
        (folder / "ps5_sync_server.inc").write_text(core + queues + cold)
        command = shlex.split(os.environ.get("CC", "cc"))
        command += shlex.split(os.environ.get("CFLAGS", "-O2 -g -Wall -Wextra -Werror"))
        command += ["-std=gnu11", "-pthread", "-I", str(folder),
                    str(ROOT / "tests/fixtures/wine_shared_sync_server.c"),
                    "-o", str(folder / "test")]
        subprocess.run(command, check=True)
        subprocess.run([str(folder / "test")], check=True, timeout=60)


if __name__ == "__main__":
    main()
