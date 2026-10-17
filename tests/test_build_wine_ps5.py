#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""The PS5 Wine patch series is well-formed and applied in numeric order."""
from __future__ import annotations
import os
import re
import shlex
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools/build_wine_ps5.sh"
PATCH = "Subject: [PATCH] test\n---\n"


def check(names: list[str], contents: str = PATCH) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory() as directory:
        for name in names:
            (Path(directory) / name).write_text(contents)
        return subprocess.run(["sh", str(SCRIPT), "--check-patches", "--patches", directory],
                              capture_output=True, text=True)


def check_patched_pe_staging() -> None:
    # Execute the real staging stanza with a fake compiler backend. This checks
    # both architecture targets, emitted bytes, stale staging cleanup and errors
    # without requiring the SDK/full Wine rebuild for this focused contract.
    text = SCRIPT.read_text()
    modules = re.search(r'^PE_MODULES="([^"\n]+)"', text, re.M).group(1)
    block = text.split('# The patched PE modules,', 1)[1].split('# The PRX link.', 1)[0]
    block = block[block.index('rm -rf'):]
    with tempfile.TemporaryDirectory() as directory:
        base = Path(directory)
        build, work = base / 'build', base / 'work'
        build.mkdir(); work.mkdir()
        stale = work / 'pe/i386-windows/stale.dll'
        stale.parent.mkdir(parents=True); stale.write_bytes(b'old')
        env = dict(os.environ, PW_PE_BUILD=str(build), PW_PE_WORK=str(work))
        setup = """
set -eu
build=$PW_PE_BUILD
work=$PW_PE_WORK
jobs=1
status=0
make() {
    for target in "$@"; do :; done
    printf '%s\n' "$target" >> "$work/requests"
    if [ "$target" = "${PW_PE_FAIL_TARGET:-}" ]; then return 17; fi
    mkdir -p "$build/$(dirname "$target")"
    printf 'MZfresh:%s' "$target" > "$build/$target"
}
"""
        script = setup + 'PE_MODULES="' + modules + '"\n' + block + '\nprintf "%s" "$status" > "$work/status"\n'
        result = subprocess.run(['sh'], input=script, env=env, capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        assert not stale.exists()
        requests = (work / 'requests').read_text().splitlines()
        for arch in ('i386', 'x86_64'):
            target = f'dlls/winevulkan/{arch}-windows/winevulkan.dll'
            assert requests.count(target) == 1
            assert (work / f'pe/{arch}-windows/winevulkan.dll').read_bytes() == ('MZfresh:' + target).encode()
        for arch in ('i386', 'x86_64'):
            for module in ('ntdll', 'win32u'):
                assert requests.count(f'dlls/{module}/{arch}-windows/{module}.dll') == 1
        assert requests.count('dlls/wow64/x86_64-windows/wow64.dll') == 1
        assert 'dlls/wow64/i386-windows/wow64.dll' not in requests
        assert (work / 'status').read_text() == '0'
        failed = 'dlls/winevulkan/i386-windows/winevulkan.dll'
        (build / failed).unlink()
        env['PW_PE_FAIL_TARGET'] = failed
        result = subprocess.run(['sh'], input=script, env=env, capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        assert (work / 'status').read_text() == '17'
        assert not (work / 'pe/i386-windows/winevulkan.dll').exists()


def main() -> int:
    check_memory_status()
    check_vk_runtime_staging()
    check_patched_pe_staging()
    # The committed series itself.
    result = subprocess.run(["sh", str(SCRIPT), "--check-patches"], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    listed = result.stdout.split()
    assert listed == sorted(p.name for p in (ROOT / "wine/patches").glob("*.patch"))
    assert listed and all(100 <= int(name[:4]) <= 999 for name in listed)
    assert check(["0900-clock.patch", "0901-input.patch", "0999-boundary.patch"]).returncode == 0
    # Numeric order across owners' ranges.
    result = check(["0500-core-b.patch", "0100-services-a.patch", "0101-services-c.patch"])
    assert result.returncode == 0 and result.stdout.split() == [
        "0100-services-a.patch", "0101-services-c.patch", "0500-core-b.patch"]
    for bad in (["0099-too-low.patch"], ["1000-too-high.patch"], ["0100-Upper.patch"],
                ["0100-a.patch", "0100-b.patch"], ["100-short.patch"], ["0100-a.diff"],
                ["notes.txt"]):
        assert check(bad).returncode != 0, bad
    assert check(["0100-no-subject.patch"], "diff --git a/x b/x\n").returncode != 0
    # libvulkan.prx comes from ps5vk or RADV, never both.
    result = subprocess.run(["sh", str(SCRIPT), "--check-patches", "--ps5vk-sdk", "/x", "--radv", "/y"],
                            capture_output=True, text=True)
    assert result.returncode != 0 and "give one" in result.stderr, result.stderr
    # The RADV link names the adapter's two entry points and writes what the
    # report audits.
    link = (ROOT / "tools/link_radv_prx.sh").read_text()
    assert "vkGetInstanceProcAddr vkGetDeviceProcAddr" in link
    assert all(name in link for name in ("libvulkan.shared.elf", "libvulkan.link.log",
                                         "sce_module/libvulkan.prx", "radv_link_recipe"))
    adapter = (ROOT / "wine/ps5/pw_vulkan_radv.c").read_text()
    assert "vk_icdGetInstanceProcAddr" in adapter and "vk_common_GetDeviceProcAddr" in adapter
    # The pinned revision matches the host runtime build.
    for tool in ("build_wine_ps5.sh", "build_wine_runtime.sh"):
        assert "WINE_COMMIT=490f6d5dcbb2a5047345b8af88d114bbcaad69a8" in (ROOT / "tools" / tool).read_text()
    print(f"wine ps5 patch series passed: {len(listed)} patch(es)")
    return 0


def check_memory_status() -> None:
    """Run the patched Wine queries and its Windows field conversion, if sourced."""
    source = Path(os.environ.get("PROSPERO_WINE_SOURCE", ROOT / ".deps/wine/source"))
    paths = ("dlls/ntdll/unix/system.c", "dlls/ntdll/unix/virtual.c")
    if not all((source / path).is_file() for path in (*paths, "dlls/kernelbase/memory.c")):
        print("wine memory status integration skipped: needs the pinned Wine source")
        return

    def function(text: str, signature: str) -> str:
        start = text.index(signature)
        end = text.index("\n{", start) + 2
        depth = 1
        while depth:
            depth += (text[end] == "{") - (text[end] == "}")
            end += 1
        return text[start:end]

    with tempfile.TemporaryDirectory() as directory:
        tree = Path(directory)
        for path in paths:
            target = tree / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text((source / path).read_text())
            for patch in sorted((ROOT / "wine/patches").glob("*.patch")):
                if f"diff --git a/{path} " in patch.read_text():
                    subprocess.run(["git", "apply", f"--include={path}", str(patch)],
                                   cwd=tree, check=True)
        perf = function((tree / paths[0]).read_text(), "static void get_performance_info(")
        basic = function((tree / paths[1]).read_text(), "void virtual_get_system_info(")
        memory = (source / "dlls/kernelbase/memory.c").read_text()
        fields = memory[memory.index("    status->dwMemoryLoad     = 0;"):]
        fields = fields[:fields.index("\n    TRACE_(virtual)")]
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#undef linux
#undef __APPLE__
#define __PROSPERO__ 1
#define max(a,b) ((a) > (b) ? (a) : (b))
#define MESSAGE(...) fprintf(stderr, __VA_ARGS__)
typedef unsigned long long ULONGLONG;
typedef uintptr_t ULONG_PTR;
typedef int BOOL;
typedef struct {
    unsigned MmHighestPhysicalPage, unknown, KeMaximumIncrement, PageSize;
    unsigned MmLowestPhysicalPage, MmNumberOfPhysicalPages, AllocationGranularity;
    void *LowestUserAddress, *HighestUserAddress;
    uintptr_t ActiveProcessorsAffinityMask;
    unsigned NumberOfProcessors;
} SYSTEM_BASIC_INFORMATION;
typedef struct {
    struct { ULONGLONG QuadPart; } IdleTime;
    unsigned AvailablePages, TotalCommittedPages, TotalCommitLimit;
} SYSTEM_PERFORMANCE_INFORMATION;
typedef struct {
    unsigned dwMemoryLoad;
    ULONGLONG ullTotalPhys, ullAvailPhys, ullTotalPageFile, ullAvailPageFile;
    ULONGLONG ullTotalVirtual, ullAvailVirtual, ullAvailExtendedVirtual;
} MEMORYSTATUSEX;
static unsigned page_size = 4096, granularity_mask = 65535;
static char *user_space_limit = (char *)0x7fff0000;
static struct { unsigned NumberOfProcessors; } peb_value = {8}, *peb = &peb_value;
static uintptr_t get_system_affinity_mask(void) { return 255; }
static char *get_wow_user_space_limit(void) { return user_space_limit; }
static ULONGLONG budget_total, budget_available;
void __wine_ps5_memory_budget(ULONGLONG *total, ULONGLONG *available)
{
    *total = budget_total;
    *available = budget_available;
}
'''
        harness += perf + "\n" + basic + r'''
static void memory_status(MEMORYSTATUSEX *status)
{
    SYSTEM_BASIC_INFORMATION basic_info;
    SYSTEM_PERFORMANCE_INFORMATION perf_info;
    struct { ULONGLONG WorkingSetSize; } vmc = {0};
    virtual_get_system_info(&basic_info, 0);
    get_performance_info(&perf_info);
'''
        harness += fields + r'''
}
int main(void)
{
    const ULONGLONG gib = 1024ull * 1024 * 1024;
    const ULONGLONG cases[][2] = {{12 * gib, 10 * gib}, {256 * 1024 * 1024, 256 * 1024 * 1024}};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        MEMORYSTATUSEX status = {0};
        budget_total = cases[i][0]; budget_available = cases[i][1];
        memory_status(&status);
        assert(status.ullTotalPhys == cases[i][0]);
        assert(status.ullAvailPhys == cases[i][1]);
        assert(status.ullTotalPageFile == 2 * cases[i][0]);
        assert(status.ullAvailPageFile == 2 * cases[i][1]);
        assert(status.ullAvailPageFile >= 0x20000000);
        assert(status.dwMemoryLoad <= 100);
    }
}
'''
        cfile, executable = tree / "memory_status.c", tree / "memory_status"
        cfile.write_text(harness)
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                       ["-std=c11", "-Wall", "-Wextra", "-Werror", str(cfile), "-o", str(executable)],
                       check=True)
        result = subprocess.run([str(executable)], capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
    print("wine memory status integration passed: physical pages, commit budget and memory load")


def check_vk_runtime_staging() -> None:
    text = SCRIPT.read_text()
    block = text.split('# Stage Vulkan batching', 1)[1].split('\ncflags=', 1)[0]
    block = block[block.index('python3'):]
    with tempfile.TemporaryDirectory() as directory:
        base = Path(directory)
        (base / 'tools').mkdir()
        source = base / 'source'
        source.mkdir()
        headers = []
        for family, module, name in [('time', 'ntdll', 'pw_qpc_clock.h'),
                                     ('input', 'win32u', 'pw_key_shared.h')]:
            if f'wine/ps5/{family}/' not in block:
                continue
            original = base / f'wine/ps5/{family}/{name}'
            original.parent.mkdir(parents=True)
            original.write_text(f'{family} shared ABI fixture\n')
            target = source / f'dlls/{module}/{name}'
            target.parent.mkdir(parents=True)
            headers.append((original, target))
        helper = base / 'tools/stage_vk_batch.py'
        helper.write_text('import sys, pathlib\nassert sys.argv[1:] == ["--source", '
                          + repr(str(source)) + ', "--repo", ' + repr(str(base))
                          + ']\npathlib.Path(sys.argv[2], "staged").write_text("paired")\n')
        setup = 'set -eu\nroot=$PW_STAGE_ROOT\ntree=$PW_STAGE_SOURCE\nfail() { exit 29; }\n'
        env = dict(os.environ, PW_STAGE_ROOT=str(base), PW_STAGE_SOURCE=str(source))
        result = subprocess.run(['sh'], input=setup + block, env=env, capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        assert (source / 'staged').read_text() == 'paired'
        for original, target in headers:
            assert target.read_bytes() == original.read_bytes()
        helper.write_text('raise SystemExit(9)\n')
        result = subprocess.run(['sh'], input=setup + block, env=env, capture_output=True, text=True)
        assert result.returncode == 29, result.stderr
    # The fingerprint covers both producer and replay sources and the staging
    # recipe; otherwise an old generated Makefile can omit a newly added unit.
    stamp = text.split('stamp=$(\n', 1)[1].split('build=$work/build', 1)[0]
    assert '"$root/tools/stage_vk_batch.py"' in stamp
    assert '"$root/tools/generate_vk_codecs.py"' in stamp
    assert '"$root"/wine/ps5/pw_vk_*.[ch]' in stamp
    assert '"$root"/wine/ps5/vulkan/*.[ch]' in stamp


if __name__ == "__main__":
    raise SystemExit(main())
