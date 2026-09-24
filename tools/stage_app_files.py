#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Stage an installed application's read-only data tree deterministically."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import sys


INVALID_WINDOWS = set('<>:"/\\|?*')


def fold_ascii(value: str) -> str:
    return value.translate(str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ",
                                        "abcdefghijklmnopqrstuvwxyz"))


def checked_component(value: str) -> str:
    if (not value or value in (".", "..") or value[-1] in (".", " ") or
            any(ord(character) < 0x20 or character in INVALID_WINDOWS
                for character in value)):
        raise ValueError(f"not a Windows-compatible path component: {value!r}")
    folded = fold_ascii(value)
    stem = folded.split(".", 1)[0].upper()
    if (stem in {"CON", "PRN", "AUX", "NUL"} or
            stem in {f"COM{index}" for index in range(1, 10)} or
            stem in {f"LPT{index}" for index in range(1, 10)}):
        raise ValueError(f"reserved Windows filename: {value!r}")
    return folded


def collect_files(source: Path) -> list[tuple[Path, tuple[str, ...]]]:
    if source.is_symlink() or not source.is_dir():
        raise ValueError("application source must be a real directory, not a symlink")
    source = source.resolve(strict=True)
    files: list[tuple[Path, tuple[str, ...]]] = []
    seen: dict[tuple[str, ...], Path] = {}
    for current, directories, filenames in os.walk(source, topdown=True,
                                                    followlinks=False):
        current_path = Path(current)
        directories.sort()
        filenames.sort()
        for name in directories:
            path = current_path / name
            if path.is_symlink():
                raise ValueError(f"application tree contains symlink: {path}")
            checked_component(name)
        for name in filenames:
            path = current_path / name
            if path.is_symlink():
                raise ValueError(f"application tree contains symlink: {path}")
            if not path.is_file():
                raise ValueError(f"application tree contains non-regular file: {path}")
            relative = path.relative_to(source)
            components = tuple(checked_component(part) for part in relative.parts)
            previous = seen.get(components)
            if previous is not None:
                raise ValueError(
                    f"case-folded path collision: {previous} and {path}")
            seen[components] = path
            files.append((path, components))
    for components, path in seen.items():
        for length in range(1, len(components)):
            ancestor = components[:length]
            if ancestor in seen:
                raise ValueError(
                    f"file/directory path collision: {seen[ancestor]} and {path}")
    return files


def stage(source: Path, destination: Path) -> int:
    files = collect_files(source)
    source_real = source.resolve(strict=True)
    destination_real = destination.resolve(strict=False)
    if os.path.commonpath((source_real, destination_real)) == str(source_real):
        raise ValueError("staging destination must not be inside the source tree")
    if destination.exists() and any(destination.iterdir()):
        raise ValueError("staging destination must be empty")
    destination.mkdir(parents=True, exist_ok=True)
    for path, components in files:
        output = destination.joinpath(*components)
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, output)
    return len(files)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    arguments = parser.parse_args()
    try:
        count = stage(arguments.source, arguments.destination)
    except (OSError, ValueError) as error:
        print(f"stage_app_files: {error}", file=sys.stderr)
        return 2
    print(f"staged {count} application files under {arguments.destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
