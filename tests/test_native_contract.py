#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Static contract for the portable core and the PS5 title.

The title cannot be compiled by `make test`: it needs the pinned Prospero
toolchain. These checks therefore hold the properties that a host compiler
would not catch anyway, and that the porting playbook says decide whether a
port survives its first boot.

The central rule is principle 1: on this firmware a platform symbol that is
merely exported is not a working one. The portable core (the DBT, profiles,
presentation) is written to import almost nothing.
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Headers the portable core is allowed to include. Anything else is a new
# dependency on the platform and must be argued for, not slipped in.
CORE_HEADERS = {"<stddef.h>", "<stdint.h>", "<string.h>", "<limits.h>"}

# Symbols the core must never reference.
#   strcasestr: its FW 12.02 provider is unusable (playbook post-mortem).
#   getcwd/chdir/access/opendir: measured EPERM or faulting from a title.
#   malloc family: the libc heap is ~8 MiB and cannot be grown.
#   snprintf/printf: the core formats nothing; the title does its own logging.
#   dlopen/execve: unavailable, and no part of this design needs them.
FORBIDDEN_CORE = (
    "strcasestr", "strcasecmp", "strncasecmp", "getcwd", "chdir", "access",
    "opendir", "readdir", "getdents", "malloc", "calloc", "realloc", "free",
    "snprintf", "sprintf", "printf", "fopen", "dlopen", "dlsym", "execve",
    "setlocale", "tolower", "toupper",
)

CORE_SOURCES = sorted(path.name for path in (ROOT / "src").glob("*.c"))


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def includes(text: str) -> list[str]:
    return re.findall(r'^\s*#include\s+(<[^>]+>)', text, re.M)


def code_without_literals_or_comments(text: str) -> str:
    """Lexical call check: documentation and string contents are not calls."""
    tokens = r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*'
    return re.sub(tokens, lambda m: ''.join('\n' if c == '\n' else ' ' for c in m[0]),
                  text, flags=re.S)


def test_forbidden_call_lexing() -> None:
    pattern = r'\bmalloc\s*\('
    for source in ('/* malloc(4) */', '// malloc(4)\n', '"malloc(4)"',
                   '"\\\"/* malloc(4) */"'):
        assert not re.search(pattern, code_without_literals_or_comments(source))
    for source in ('malloc(4)', '"/*"; malloc(4)', '/* docs */ malloc /* gap */ (4)'):
        assert re.search(pattern, code_without_literals_or_comments(source))


def test_core_imports_nothing_surprising() -> None:
    for name in CORE_SOURCES + [path.name for path in (ROOT / "src").glob("*.h")]:
        relative = f"src/{name}"
        text = read(relative)
        if name in ("pw_vm_posix.c", "pw_vm_posix.h"):
            continue                    # the one deliberate POSIX backend
        for header in includes(text):
            assert header in CORE_HEADERS, f"{relative} includes {header}"
        code = code_without_literals_or_comments(text)
        for symbol in FORBIDDEN_CORE:
            assert not re.search(rf"\b{symbol}\s*\(", code), \
                f"{relative} calls {symbol}"


def test_x87_never_uses_host_floating_point_state() -> None:
    code = code_without_literals_or_comments(read("src/pw_x87.c"))
    assert not re.search(r"\b(float|double)\b", code)
    assert not re.search(r"\b(__asm__|asm)\b", code)
    for symbol in ("sqrt", "sin", "cos", "fenv", "fesetround"):
        assert not re.search(rf"\b{symbol}\s*\(", code), symbol


def test_posix_backend_is_narrow() -> None:
    text = read("src/pw_vm_posix.c")
    assert set(includes(text)) <= {"<sys/mman.h>", "<unistd.h>"}, \
        includes(text)
    # It must never fall back to a file-backed mapping: PS5 mmap of a file
    # returns ENOTSUP, so an accidental dependency would only fail on target.
    assert "MAP_ANONYMOUS" in text
    assert "MAP_SHARED" not in text
    assert "open(" not in text


def test_builder_builds_the_wine64_title() -> None:
    builder = read("tools/build_native.sh")
    sources = builder[builder.index("sources=("):]
    sources = sources[:sources.index(")")]
    listed = re.findall(r"[\w/]+\.c", sources)
    assert listed[0] == "native/wine64_main.c"
    for name in listed:
        assert (ROOT / name).is_file(), f"tools/build_native.sh compiles missing {name}"
    for name in ("native/pw_audio_ps5.c", "native/pw_agc_submit_lifecycle.c",
                 "native/pw_data_mount.c", "native/pw_wine_library.c"):
        assert name in sources, name
    # The banned import is rejected by the build, not merely documented.
    assert "strcasestr" in builder

def test_agc_submit_establishes_a_suspend_point() -> None:
    adapter = read("native/pw_agc_ps5.c")
    stub = read("native/stubs/libSceAgc.c")
    assert "sceAgcSuspendPoint" in adapter
    assert "pw_agc_submit_and_suspend(&submit,sceAgcDriverSubmitDcb," in adapter
    assert "sceAgcSuspendPoint);" in adapter
    assert "int32_t sceAgcSuspendPoint(void)" in stub


def main() -> int:
    tests = [value for name, value in sorted(globals().items())
             if name.startswith("test_") and callable(value)]
    for test in tests:
        test()
    print(f"native contract passed: {len(tests)} checks, "
          f"{len(CORE_SOURCES)} core sources")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
