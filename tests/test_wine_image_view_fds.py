#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Bounded native checks of the actual image-view resource cleanup patch bodies."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "wine/patches/0870-server-ps5-release-image-view-fds.patch"
DEFAULT_PATCH = ROOT / "wine/patches/0880-wine-ps5-runtime-defaults.patch"


def patch_new_sides():
    files = {}
    current = None
    for line in PATCH.read_text().splitlines(keepends=True):
        if line.startswith("+++ b/"):
            current = line[6:].strip()
            files.setdefault(current, [])
        elif line.startswith(("---", "@@")):
            continue
        elif current and line.startswith(("+", " ")):
            files[current].append(line[1:])
        elif current and line == "\n":
            files[current].append(line)
    return {name: "".join(lines) for name, lines in files.items()}


def function(source, name):
    match = re.search(r"(?:static\s+)?(?:int|void|struct\s+\w+\s*\*)\s*" +
                      re.escape(name) + r"\s*\([^;{}]*\)\s*\{", source)
    assert match, f"missing complete patched body: {name}"
    start = source.index("{", match.start())
    depth = 0
    for index in range(start, len(source)):
        depth += (source[index] == "{") - (source[index] == "}")
        if not depth:
            return source[match.start():index + 1] + "\n"
    raise AssertionError(f"unterminated function: {name}")


def apply_default_selection(source):
    """Apply the actual FD selection hunk to the reconstructed 0870 body."""
    part = DEFAULT_PATCH.read_text().split("diff --git a/server/fd.c b/server/fd.c\n")[1]
    for hunk in part.split("@@")[2::2]:
        lines = hunk.splitlines(keepends=True)[1:]
        before = "".join(line if line == "\n" else line[1:] for line in lines
                         if line == "\n" or line.startswith((" ", "-")))
        after = "".join(line if line == "\n" else line[1:] for line in lines
                        if line == "\n" or line.startswith((" ", "+")))
        assert before and source.count(before) == 1, "default hunk no longer matches 0870"
        source = source.replace(before, after)
    return source


def main():
    sources = patch_new_sides()
    fd_source = apply_default_selection(sources["server/fd.c"])
    mapping_source = sources["server/mapping.c"]
    refs = re.search(r"struct image_view_fd_refs\s*\{[^}]*\};", mapping_source)
    assert refs
    bodies = [function(fd_source, name) for name in (
        "ps5_image_view_fd_release_enabled", "ps5_release_image_view_fd",
        "ps5_is_image_view_fd_released", "get_fd_object_for_mapping")]
    bodies.append(refs.group(0))
    bodies.extend(function(mapping_source, name) for name in (
        "count_image_view_fd_refs", "release_mapping_image_view_fd",
        "mapping_destroy", "get_view_file"))
    template = (ROOT / "tests/fixtures/wine_image_view_fds.c").read_text()
    assert template.count("/* PATCH_BODIES */") == 1
    environment = dict(os.environ)
    environment.pop("WINE_PS5_IMAGE_VIEW_FD_RELEASE", None)
    with tempfile.TemporaryDirectory(prefix="pw-image-view-fds-") as temp:
        directory = Path(temp)
        source = directory / "fixture.c"
        source.write_text(template.replace("/* PATCH_BODIES */", "\n".join(bodies)))
        executable = directory / "fixture"
        compiler = shlex.split(os.environ.get("CC", "cc"))
        flags = shlex.split(os.environ.get("CFLAGS", "-O2 -Wall -Wextra -Werror"))
        subprocess.run([*compiler, "-std=c11", "-D__PROSPERO__", *flags,
                        str(source), "-o", str(executable)], check=True, timeout=60)
        subprocess.run([str(executable)], env=environment, check=True, timeout=20)
        # Missing/invalid prefix directory remains off; explicit 0 also wins.
        subprocess.run([str(executable), "default-off"], env=environment, check=True, timeout=20)
        selected = dict(environment, WINE_PS5_IMAGE_VIEW_FD_RELEASE="0")
        subprocess.run([str(executable), "default-off"], env=selected, check=True, timeout=20)
        switch_file = directory / "pw_image_view_fd_release"
        checks = 0
        for content, expected in [(None, 1), (b"", 0), (b"1", 1), (b"1\n", 1),
                                  (b"0", 0), (b"true", 0), (b"1\x00", 0),
                                  (b"1\n\n", 0), (b"1extra", 0), (b" 1", 0)]:
            if switch_file.exists():
                switch_file.unlink()
            if content is not None:
                switch_file.write_bytes(content)
            subprocess.run([str(executable), "switch", str(directory), str(expected)],
                           env=environment, check=True, timeout=10, stderr=subprocess.PIPE)
            checks += 1
        switch_file.unlink()
        # A directory causes a read error; a self-link causes a non-ENOENT open error.
        switch_file.mkdir()
        subprocess.run([str(executable), "switch", str(directory), "0"],
                       env=environment, check=True, timeout=10, stderr=subprocess.PIPE)
        switch_file.rmdir()
        switch_file.symlink_to(switch_file.name)
        subprocess.run([str(executable), "switch", str(directory), "0"],
                       env=environment, check=True, timeout=10, stderr=subprocess.PIPE)
        switch_file.unlink()
        checks += 2
        switch_file.write_bytes(b"1\n")
        for value, expected in [("1", 1), ("0", 0), ("", 0), ("true", 0), ("1\n", 0)]:
            selected = dict(environment, WINE_PS5_IMAGE_VIEW_FD_RELEASE=value)
            subprocess.run([str(executable), "switch", str(directory), str(expected)],
                           env=selected, check=True, timeout=10, stderr=subprocess.PIPE)
            checks += 1
        print(f"Strict default-on/file/environment/cached/error selection: {checks} PASS")
        # The option must have no effect on non-PS5 builds, including an explicit 1.
        subprocess.run([*compiler, "-std=c11", *flags, str(source), "-o", str(executable)],
                       check=True, timeout=60)
        selected = dict(environment, WINE_PS5_IMAGE_VIEW_FD_RELEASE="1")
        subprocess.run([str(executable), "default-off"], env=selected, check=True, timeout=20)


if __name__ == "__main__":
    main()
