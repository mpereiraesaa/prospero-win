#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Compile exact native ABI/client bodies with bounded legal fixture state.

This does not run Wine or establish asynchronous/console semantics or speed.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "wine/patches/0887-ntdll-ps5-shared-sync-client.patch"


def additions(patch, file):
    part = next(part for part in patch.read_text().split("diff --git ")[1:]
                if part.splitlines()[0].endswith(" b/" + file))
    return "\n".join(line[1:] for line in part.splitlines()
                     if line.startswith("+") and not line.startswith("+++")) + "\n"


def new_side(patch, file):
    part = next(part for part in patch.read_text().split("diff --git ")[1:]
                if part.splitlines()[0].endswith(" b/" + file))
    return "\n".join(line[1:] for line in part.splitlines()
                     if line.startswith(("+", " ")) and not line.startswith("+++")) + "\n"


def body(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    for end in range(brace + 1, len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if not depth:
            return source[start:end + 1] + "\n"
    raise AssertionError("unterminated actual function")


def main():
    client = new_side(PATCH, "dlls/ntdll/unix/server.c")
    start = client.index("/* Exact canonical handles,")
    cache = client[start:client.index("\n#endif", start)]
    wrapper = body(client, "unsigned int server_try_shared_sync(")
    switch = body(client, "static int server_shared_sync_enabled(")
    server = new_side(PATCH, "server/request.c")
    assert "server_clear_shared_sync_slot(handle);" in client
    # The existing mutex clear is reached at all four actual close/cache sites.
    mutex = ROOT / "wine/patches/0820-ntdll-ps5-shared-mutex-client.patch"
    for file, expected in [("dlls/ntdll/unix/server.c", 3), ("dlls/ntdll/unix/thread.c", 1)]:
        part = next(part for part in mutex.read_text().split("diff --git ")[1:]
                    if part.splitlines()[0].endswith(" b/" + file))
        lines = part.splitlines()
        pairs = [(lines[i - 1], line) for i, line in enumerate(lines)
                 if line.startswith(" ") and "close_inproc_sync( " in line]
        assert len(pairs) == expected
        assert all("server_clear_shared_mutex_slot(" in prior for prior, _ in pairs)
    calls = additions(PATCH, "dlls/ntdll/unix/sync.c")
    assert calls.count("server_try_shared_sync(") == 4
    assert "!alertable &&" in calls and "PW_SYNC_WAIT" in calls
    assert "pw_wineserver_sync_backend" in (ROOT / "tools/build_wine_ps5.sh").read_text()
    with tempfile.TemporaryDirectory(prefix="pw-shared-sync-client-") as temp:
        folder = Path(temp)
        (folder / "ps5_sync_word.h").write_text(additions(
            ROOT / "wine/patches/0885-server-ps5-shared-sync-word.patch", "include/wine/ps5_sync_word.h"))
        (folder / "ps5_sync_backend.h").write_text(additions(PATCH, "include/wine/ps5_sync_backend.h"))
        (folder / "shared_sync_client.inc").write_text(cache + "\n" + wrapper)
        (folder / "shared_sync_switch.inc").write_text(switch)
        (folder / "shared_sync_server_abi.inc").write_text(
            body(server, "static int get_shared_sync_word(") +
            body(server, "DECLSPEC_EXPORT const struct pw_sync_backend *pw_wineserver_sync_backend("))
        compiler = shlex.split(os.environ.get("CC", "cc"))
        flags = shlex.split(os.environ.get("CFLAGS", "-O2 -g -Wall -Wextra -Werror"))
        command = [*compiler, *flags, "-std=gnu11", "-pthread", "-I", str(folder),
                   str(ROOT / "tests/fixtures/wine_shared_sync_client.c"), "-o", str(folder / "test")]
        subprocess.run(command, check=True, timeout=60)
        subprocess.run([str(folder / "test"), str(folder)], check=True, timeout=60)
        # Ordinary builds have no shared backend: the actual wrapper falls back.
        stub = folder / "stub.c"
        stub.write_text("#include <assert.h>\n#include <stddef.h>\n"
                        "typedef void *HANDLE; typedef long LARGE_INTEGER;\n"
                        "#define STATUS_NOT_IMPLEMENTED 0xc0000002u\n" + wrapper +
                        "int main(void) { unsigned old=91; assert(server_try_shared_sync(NULL,0,0,NULL,&old)"
                        "==STATUS_NOT_IMPLEMENTED && old==91); return 0; }\n")
        subprocess.run([*compiler, *flags, "-Wno-unused-parameter", str(stub), "-o", str(folder / "stub")],
                       check=True, timeout=60)
        subprocess.run([str(folder / "stub")], check=True, timeout=10)


if __name__ == "__main__":
    main()
