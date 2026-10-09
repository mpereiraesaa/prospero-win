#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""The PS5 Wine patch series is well-formed and applied in numeric order."""
from __future__ import annotations
import os
import re
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
        for module in ('wow64', 'wow64win'):
            target = f'dlls/{module}/x86_64-windows/{module}.dll'
            assert requests.count(target) == 1
            assert (work / f'pe/x86_64-windows/{module}.dll').read_bytes() == ('MZfresh:' + target).encode()
            assert f'dlls/{module}/i386-windows/{module}.dll' not in requests
        assert (work / 'status').read_text() == '0'
        failed = 'dlls/winevulkan/i386-windows/winevulkan.dll'
        (build / failed).unlink()
        env['PW_PE_FAIL_TARGET'] = failed
        result = subprocess.run(['sh'], input=script, env=env, capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        assert (work / 'status').read_text() == '17'
        assert not (work / 'pe/i386-windows/winevulkan.dll').exists()


def main() -> int:
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


def check_vk_runtime_staging() -> None:
    text = SCRIPT.read_text()
    block = text.split('# Stage Vulkan batching', 1)[1].split('# The PS5 OpenGL SDK', 1)[0]
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
