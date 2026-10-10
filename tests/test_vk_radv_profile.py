#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""The RADV entry-point profile (wine/ps5/pw_vk_radv_profile*): the tables
and lines on the host, with sanitizers; the wrappers and the loader shim
compiled against a Vulkan header when a recent enough one is found (the
RADV release's, named by PW_VULKAN_INCLUDE, or the system's), skipped
otherwise; and the
generated list: every entry is a device function with a parameter list
whose names match its argument list, in a stable order."""
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "wine/ps5"
ENTRY = re.compile(r"^PW_VK_RADV_FN(?:_VOID|_HAND)?\((vk\w+), (?:(\w+), )?\((.*?)\), \((.*?)\)\)$")


def check_list() -> int:
    names = []
    for line in (BASE / "pw_vk_radv_profile_list.h").read_text().splitlines():
        if not line.startswith("PW_VK_RADV_FN"):
            continue
        match = ENTRY.match(line)
        assert match, line
        name, ret, params, args = match.groups()
        assert line.startswith("PW_VK_RADV_FN_VOID") == (ret is None), line
        expected = [re.sub(r"\[.*\]", "", p.split()[-1].lstrip("*")) for p in params.split(", ")]
        assert expected == args.split(", "), line
        first = params.split(", ")[0].split()[0]
        assert first in ("VkDevice", "VkCommandBuffer", "VkQueue"), line
        names.append(name)
    assert len(names) == len(set(names)) and len(names) > 100, len(names)
    for required in ("vkCmdDraw", "vkCmdDrawIndexed", "vkCmdBindPipeline", "vkCmdBindDescriptorSets",
                     "vkUpdateDescriptorSetWithTemplate", "vkCmdPipelineBarrier2", "vkCreateGraphicsPipelines",
                     "vkAllocateMemory", "vkQueueSubmit2", "vkQueuePresentKHR", "vkWaitForFences"):
        assert required in names, required
    hand = [line for line in (BASE / "pw_vk_radv_profile_list.h").read_text().splitlines()
            if line.startswith("PW_VK_RADV_FN_HAND(")]
    wrap = (BASE / "pw_vk_radv_profile_wrap.c").read_text()
    for line in hand:
        name = ENTRY.match(line).group(1)
        assert f"wrap_{name}(" in wrap, f"no hand-written wrapper for {name}"
    return len(names)


HEADER_VERSION = re.compile(r"#define VK_HEADER_VERSION (\d+)")
# The list uses the names Vulkan 1.4 promoted (VkSubresourceLayout2 and the
# like), so an older header cannot compile the wrappers.
MIN_HEADER_VERSION = 303


def vulkan_include() -> Path | None:
    named = os.environ.get("PW_VULKAN_INCLUDE")
    for candidate in ([Path(named)] if named else []) + [Path("/usr/include")]:
        core = candidate / "vulkan/vulkan_core.h"
        if not (candidate / "vulkan/vulkan.h").is_file() or not core.is_file():
            continue
        match = HEADER_VERSION.search(core.read_text())
        if match and int(match[1]) >= MIN_HEADER_VERSION:
            return candidate
    return None


def main() -> int:
    cc = shutil.which("cc")
    assert cc
    count = check_list()
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", f"-I{BASE}"]
    with tempfile.TemporaryDirectory(prefix="pw-vk-radv-profile-") as directory:
        out = Path(directory)
        for name, extra in [("host", ["-O2"]),
                            ("sanitized", ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]:
            subprocess.run([cc, *flags, *extra, str(BASE / "pw_vk_radv_profile.c"),
                            str(ROOT / "tests/test_vk_radv_profile.c"), "-o", str(out / name)], check=True)
            run = subprocess.run([str(out / name)], capture_output=True, text=True, check=True)
            assert "PASS" in run.stdout, run.stdout
        include = vulkan_include()
        if include:
            for unit in ("pw_vk_radv_profile_wrap.c", "pw_vulkan_radv.c"):
                subprocess.run([cc, *flags, "-O2", f"-I{include}", "-fsyntax-only", str(BASE / unit)], check=True)
            print(f"RADV profile: {count} entry points, wrappers compile against {include}/vulkan")
        else:
            print(f"RADV profile: {count} entry points; no Vulkan header {MIN_HEADER_VERSION} or newer, "
                  "wrappers not compiled")
    return 0


if __name__ == "__main__":
    sys.exit(main())
