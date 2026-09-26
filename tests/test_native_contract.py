#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Static contract for the loader core and the PS5 adapter.

The adapter cannot be compiled by `make test`: it needs the pinned Prospero
toolchain. These checks therefore hold the properties that a host compiler
would not catch anyway, and that the porting playbook says decide whether a
port survives its first boot.

The central rule is principle 1: on this firmware a platform symbol that is
merely exported is not a working one. The loader core is written to import
almost nothing, and the calls the adapter does depend on are the ones the
laboratory has already measured, each covered by a boot-time smoke test.
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
#   snprintf/printf: formatting belongs to the gate's own bounded helpers.
#   dlopen/execve: unavailable, and no part of this design needs them.
FORBIDDEN_CORE = (
    "strcasestr", "strcasecmp", "strncasecmp", "getcwd", "chdir", "access",
    "opendir", "readdir", "getdents", "malloc", "calloc", "realloc", "free",
    "snprintf", "sprintf", "printf", "fopen", "dlopen", "dlsym", "execve",
    "setlocale", "tolower", "toupper",
)

CORE_SOURCES = sorted(
    path.name for path in (ROOT / "src").glob("*.c")
    if path.name != "pw_file_posix.c"
)


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


def test_module_classification_is_explicit() -> None:
    text = read("src/pw_module_name.c")
    # The Win32 surface prospero-win implements itself, never loads.
    for name in ("kernel32.dll", "user32.dll", "msvcrt.dll", "ntdll.dll"):
        assert f'"{name}"' in text, name
    # Third-party modules must stay absent so they are manually mapped.
    for name in ("binkw32.dll", "mss32.dll", "smackw32.dll"):
        assert f'"{name}"' not in text, name


def test_adapter_uses_measured_platform_calls() -> None:
    text = read("native/pw_file_ps5.c")
    # sceKernelOpen/Close/Stat plus libc read/lseek on that descriptor is
    # the combination already proven on FW 12.02 by the Xash3D port.
    for symbol in ("sceKernelOpen", "sceKernelClose", "sceKernelStat"):
        assert f"extern int {symbol}(" in text, symbol
    assert re.search(r"\bread\(descriptor", text)
    assert re.search(r"\blseek\(descriptor", text)
    # The image cannot be listed from a title, so there is no scan here.
    for symbol in ("opendir", "readdir", "sceKernelGetdents"):
        assert symbol not in text, symbol
    # File bytes never come from the libc heap.
    assert "mmap(" in text
    assert re.search(r"\bmalloc\(", text) is None


def test_compat32_adapter_asks_the_kernel_correctly() -> None:
    text = read("native/pw_compat32_ps5.c")
    # The syscall this gate exists to measure, named explicitly.
    assert "extern int sysarch(int number, void *args);" in text
    assert "PW_I386_SET_LDT = 1" in text
    assert "PW_LDT_AUTO_ALLOC = 0xffffffff" in text
    # FreeBSD packs the descriptor pointer at offset 4 on amd64. Getting it
    # wrong hands the kernel a garbage pointer, so it is asserted at compile
    # time rather than trusted.
    assert text.count("_Static_assert") >= 3
    assert "offsetof(struct pw_ldt_args, descs) == 4" in text
    # The probe must never ask for memory that is writable and executable at
    # once: it writes the stub, then seals the page.
    assert "PROT_WRITE | PROT_EXEC" not in text
    assert "PROT_READ | PROT_EXEC" in text
    # MAP_FIXED silently replaces live mappings on this firmware, and
    # MAP_EXCL is ignored there, so a low address is requested with a hint
    # and verified afterwards, never demanded. Matched on the flag
    # combination rather than the bare word, which appears in the comment
    # explaining exactly this.
    assert "MAP_ANONYMOUS | MAP_FIXED" not in text
    assert "MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)" in text
    assert "(uintptr_t)page != (uintptr_t)base" in text


def test_compat32_stub_stays_below_two_gib() -> None:
    text = read("src/pw_compat32.c")
    # `mov rsp, imm32` sign-extends and every far pointer holds a 32-bit
    # offset, so both pages must sit in the low 2 GiB.
    assert "base < 0x80000000u" in text
    # The mode proof must be the three bytes that only decode as `inc eax`
    # in 32-bit mode; a plain magic constant would prove nothing.
    assert text.count("0x40,") >= 3
    assert "PW_COMPAT32_EXPECTED_RESULT" in read("src/pw_compat32.h")


def test_adapter_reports_before_it_parses() -> None:
    text = read("native/main.c")
    assert "PW_BEGIN" in text
    # A platform pre-flight runs before the loader touches any bytes.
    smoke = text.index("pw_file_ps5_smoke")
    gate = text.index("pw_gate_run")
    assert smoke < gate, "the filesystem smoke test must precede the loader"
    assert "PW_FS_SMOKE" in text
    # Every exit path closes the channel, so no run ends without a BYE,
    # including the fault path: a crash that says nothing costs a whole run.
    assert "install_signal_reporter();" in text
    assert "PW_SIGNAL" in text and "pc_minus_main" in text
    assert "SA_SIGINFO" in text
    # Count statements, not prose: the file also discusses _exit() in a
    # comment explaining why main() must never return on this firmware.
    exits = len(re.findall(r"^\s*_exit\(", text, re.M))
    closes = len(re.findall(r"^\s*ps5log_close\(", text, re.M))
    assert exits == closes, (exits, closes)
    assert exits >= 6
    # The registry is far too large for the libc heap.
    assert "reserve_scratch(sizeof(*loader))" in text
    assert re.search(r"\bmalloc\(", text) is None
    # Gate 0.2a runs before the loader and its stage-two transfer is opt-in.
    probe = text.index("pw_compat32_probe")
    assert probe < gate, "the compatibility-mode probe must precede the loader"
    assert "PW_COMPAT32_TRANSFER" in text


def test_builder_compiles_every_core_source() -> None:
    builder = read("tools/build_native.sh")
    sources = builder[builder.index("sources=("):]
    sources = sources[:sources.index(")")]
    for name in CORE_SOURCES:
        assert f"src/{name}" in sources, \
            f"tools/build_native.sh does not compile src/{name}"
    # The host-only provider depends on dirent and stdio, both unusable on
    # the console image; linking it in would only fail on target.
    assert "pw_file_posix.c" not in sources
    assert 'entry=native/runtime_main.c' in builder
    assert '[[ $native_mode == gate ]] && entry=native/main.c' in builder
    assert '"$entry"' in sources
    assert "native/pw_file_ps5.c" in sources
    assert "native/pw_audio_ps5.c" in sources
    assert "native/pw_agc_submit_lifecycle.c" in sources
    # The banned import is rejected by the build, not merely documented.
    assert "strcasestr" in builder


def test_runtime_entry_owns_execution_services() -> None:
    text = read("native/runtime_main.c")
    for symbol in ("pw_x86_engine_step", "pw_win32_dispatch",
                   "pw_gdi_target_view", "pw_audio_ps5_submit",
                   "pw_pad_ps5_poll", "pw_user32_post_quit"):
        assert symbol in text, symbol
    assert "pad->core.pressed_edges&PAD_CREATE" in text
    # The launcher closes with a held combo, keeps pad and display across
    # sessions and never opens the display from inside a launched session.
    assert "const uint32_t combo=PAD_OPTIONS|PAD_CREATE;" in text
    assert "now-combo_since>=1000000000ull" in text
    assert 'PW_PAD_QUIT schema=1 source=combo action=WM_QUIT' in text
    assert "host->launcher?PW_OK:pw_pad_ps5_close(" in text
    assert "if(!host->launcher && (status=host_open_display(host))!=PW_OK)" in text
    assert 'PW_PAD_QUIT schema=1 source=create action=WM_QUIT' in text
    assert 'PAD_CREATE,0x1b' not in text
    assert "PW_RUNTIME_READY" in text
    assert "PW_RUNTIME_HEARTBEAT" in text
    assert "PW_EXEC_ABORT schema=1" in text
    assert "eip=0x%08x" in text and "bytes=%02x%02x%02x%02x" in text
    assert "for(;;events++)" in text
    assert "ps5log_close(\"runtime-signal\")" in text


def test_native_wine_bootstrap_is_opt_in_and_bounded() -> None:
    builder = read("tools/build_native.sh")
    entry = read("native/wine_main.c")
    assert "native/wine_main.c" in builder
    assert "PW_NATIVE_MODE=wine" in builder
    assert "PW_WINE_RUNTIME_DIR is required" in builder
    assert "PW_APP_PROFILE is required for PW_NATIVE_MODE=wine" in builder
    assert '"$dist/win/app/app.profile"' in builder
    assert "--root-module" in builder
    assert "PW_USE_APP_PROFILE" in entry
    assert "app_profile.graphics != PW_APP_GRAPHICS_GDI" in entry
    host_entry = read("tools/wine_ntdll_entry.c")
    assert "app_profile.graphics != PW_APP_GRAPHICS_GDI" in host_entry
    assert "PW_OUTPUT_SUFFIX" in builder
    assert "--application" in builder
    assert 'PW_STAGE_DIR "/runtime/lib/i386-windows"' in entry
    assert 'PW_STAGE_DIR "/runtime/nls"' in entry
    for contract in ("pw_wine_seed_registry", "pw_wine_seed_objects",
                     "pw_wine_seed_user_sid", "pw_file_ps5_wine_file_service",
                     "LdrInitializeThunk", "host_calls=%llu", "ps5log_close"):
        assert contract in entry, contract
    assert '"FreeBSD"' in entry and '"PS5"' in entry


def test_prefix_registry_names_match_pinned_wine_contract() -> None:
    header = read("src/pw_prefix.h")
    source = read("src/pw_prefix.c")
    docs = read("docs/WINE_INTEGRATION.md")
    assert "PW_PREFIX_HIVE_COUNT = 3" in header
    for name in ("system.reg", "user.reg", "userdef.reg"):
        assert f'"{name}"' in source
        assert name in docs
    assert "classes.reg" not in source
    assert "no separate `classes.reg`" in docs


def test_agc_submit_establishes_a_suspend_point() -> None:
    adapter = read("native/pw_agc_ps5.c")
    stub = read("native/stubs/libSceAgc.c")
    assert "sceAgcSuspendPoint" in adapter
    assert "pw_agc_submit_and_suspend(&submit,sceAgcDriverSubmitDcb," in adapter
    assert "sceAgcSuspendPoint);" in adapter
    assert "int32_t sceAgcSuspendPoint(void)" in stub


def test_gate_records_stay_within_the_transport_budget() -> None:
    text = read("src/pw_gate.h")
    line_max = int(re.search(r"PW_GATE_LINE_MAX = (\d+)", text).group(1))
    protocol = read("native/ps5log/ps5log.h")
    record_max = int(re.search(r"define PS5LOG_MAX_LINE (\d+)",
                               protocol).group(1))
    # An oversized record would be truncated by the transport and the
    # manifest would report it, failing the run for a formatting reason.
    assert line_max < record_max, (line_max, record_max)


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
