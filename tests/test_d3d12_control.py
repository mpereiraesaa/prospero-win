#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Direct3D 12 runs as pw_install sets it up: DXVK's dxgi and vkd3d-proton's
d3d12 and d3d12core, extracted by pw_install's own code from the pinned
releases, under the host Wine. tests/d3d12_control.c creates a device at
feature level 11_0, reports what it offers, and draws, reads back and presents
two frames on a flip-model swapchain.

It needs a Vulkan GPU, the host Wine from tools/build_host_wine.sh
(PROSPERO_HOST_WINE, default .deps/wine-host/install/usr/bin/wine),
x86_64-w64-mingw32-gcc and the two releases (downloaded into pw_install's
cache once), so it runs only with PROSPERO_TEST_D3D12=1 and is skipped
otherwise. The same program, built here, is the console's check: run it as
a game in a prefix pw_install set up with `wine: {dxvk: true, vkd3d: true}`."""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tests" / "d3d12_control.c"
sys.path.insert(0, str(ROOT / "tools"))
import pw_install  # noqa: E402

DXVK_VERSION = "2.6.2"
VKD3D_VERSION = "3.0.1"


def build(compiler: str, exe: Path) -> None:
    subprocess.run([compiler, "-O1", "-Wall", "-Wextra", "-Werror", "-o", str(exe), str(SOURCE),
                    "-ld3d12", "-ldxgi", "-ld3dcompiler_47"], check=True)


def main() -> int:
    wine = os.environ.get("PROSPERO_HOST_WINE", str(ROOT / ".deps/wine-host/install/usr/bin/wine"))
    compiler = shutil.which("x86_64-w64-mingw32-gcc")
    if os.environ.get("PROSPERO_TEST_D3D12") != "1":
        if compiler:   # the program still has to build
            with tempfile.TemporaryDirectory() as directory:
                build(compiler, Path(directory) / "d3d12_control.exe")
            print("d3d12 control built; run skipped (PROSPERO_TEST_D3D12=1 runs it on a Vulkan GPU)")
        else:
            print("d3d12 control skipped: needs x86_64-w64-mingw32-gcc")
        return 0
    assert os.access(wine, os.X_OK) and compiler, "needs the host Wine and x86_64-w64-mingw32-gcc"

    with tempfile.TemporaryDirectory() as directory:
        exe = Path(directory) / "d3d12_control.exe"
        build(compiler, exe)
        env = dict(os.environ, WINEPREFIX=str(Path(directory) / "pfx"), WINEDEBUG="-all",
                   WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d")
        subprocess.run([wine, "wineboot", "-i"], env=env, check=True, capture_output=True)
        windows = Path(env["WINEPREFIX"]) / "drive_c" / "windows"
        url, digest = pw_install.dxvk_release(DXVK_VERSION)
        pw_install.extract_dxvk(pw_install.fetch(url, digest, f"dxvk-{DXVK_VERSION}.tar.gz"), DXVK_VERSION, windows)
        url, digest = pw_install.vkd3d_proton_release(VKD3D_VERSION)
        pw_install.extract_vkd3d_proton(pw_install.fetch(url, digest, url.rsplit("/", 1)[-1]), VKD3D_VERSION,
                                        windows)
        env["WINEDLLOVERRIDES"] += ";dxgi,d3d12,d3d12core=n"
        # vkd3d-proton keeps its pipeline cache in the current directory.
        done = subprocess.run([wine, str(exe)], capture_output=True, text=True, env=env, timeout=300,
                              cwd=directory)
        subprocess.run([str(Path(wine).with_name("wineserver")), "-k"], env=env, capture_output=True)
        line = next((text for text in done.stdout.splitlines() if text.startswith("D3D12_CONTROL ")), "")
        assert line, f"no result line:\n{done.stdout}{done.stderr[-4000:]}"
        assert done.returncode == 0 and line.endswith("verdict=pass"), line
        level = re.search(r"max_level=(0x[0-9a-f]+)", line).group(1)
        model = re.search(r"shader_model=(0x[0-9a-f]+)", line).group(1)
    print(f"d3d12 control passed: device, feature level {level}, shader model {model}, "
          "two frames drawn, read back and presented")
    return 0


if __name__ == "__main__":
    sys.exit(main())
