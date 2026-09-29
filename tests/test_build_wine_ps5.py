#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""The PS5 Wine patch series is well-formed and applied in numeric order."""
from __future__ import annotations
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


def main() -> int:
    # The committed series itself.
    result = subprocess.run(["sh", str(SCRIPT), "--check-patches"], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    listed = result.stdout.split()
    assert listed == sorted(p.name for p in (ROOT / "wine/patches").glob("*.patch"))
    assert listed and all(100 <= int(name[:4]) <= 899 for name in listed)
    # Numeric order across both owners' ranges.
    result = check(["0500-core-b.patch", "0100-services-a.patch", "0101-services-c.patch"])
    assert result.returncode == 0 and result.stdout.split() == [
        "0100-services-a.patch", "0101-services-c.patch", "0500-core-b.patch"]
    for bad in (["0099-too-low.patch"], ["0900-too-high.patch"], ["0100-Upper.patch"],
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


if __name__ == "__main__":
    raise SystemExit(main())
