#!/usr/bin/env python3
"""Exercise the exact identity resolver shipped by the native signal patch."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
patch = root / "wine/patches/0883-ntdll-ps5-native-wow64-signal-entry.patch"
with tempfile.TemporaryDirectory(prefix="native-provider-test-") as directory:
    out = Path(directory)
    files = {}
    current = None
    for line in patch.read_text().splitlines():
        if line.startswith("diff --git "):
            current = None
        if line.startswith("+++ b/dlls/ntdll/unix/native_libkernel_resolver."):
            current = line.rsplit("/", 1)[1]
            files[current] = []
        elif current and line.startswith("+") and not line.startswith("+++"):
            files[current].append(line[1:])
    assert set(files) == {"native_libkernel_resolver.c", "native_libkernel_resolver.h"}
    for name, lines in files.items():
        (out / name).write_text("\n".join(lines) + "\n")
    executable = out / "resolver-test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                    "-Werror", "-I" + str(out), str(out / "native_libkernel_resolver.c"),
                    str(root / "tests/native_libkernel_resolver_fixture.c"),
                    "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
