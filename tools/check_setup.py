#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Check host contributor prerequisites; --wine also checks Wine build tools."""
import argparse
import importlib.util
import platform
import shutil


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wine", action="store_true")
    args = parser.parse_args()
    missing = []
    if platform.system() != "Linux" or platform.machine() not in ("x86_64", "AMD64"):
        missing.append("x86_64 Linux (native or a Linux VM/WSL2)")
    # i686-w64-mingw32-gcc: the 32-bit Vulkan batching checks build its PE side.
    for command in ("git", "make", "cc", "clang", "i686-w64-mingw32-gcc") + (("bison", "flex", "pkg-config",
                           "x86_64-w64-mingw32-gcc", "i686-w64-mingw32-strip", "x86_64-w64-mingw32-strip") if args.wine else ()):
        if not shutil.which(command): missing.append(command)
    if importlib.util.find_spec("yaml") is None: missing.append("Python PyYAML (python3-yaml or pyyaml)")
    if missing:
        print("Missing prerequisites:\n" + "\n".join("- " + item for item in missing))
        print("See docs/DEVELOPMENT.md#host-setup for installation commands.")
        return 1
    print("Host prerequisites found. Run make -j2 all and make -j2 sanitize.")
    if args.wine: print("Wine configure also checks optional graphics/audio headers; inspect its warnings.")
    return 0


if __name__ == "__main__": raise SystemExit(main())
