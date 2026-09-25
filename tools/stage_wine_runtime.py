#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Validate and copy one pinned i386 Wine runtime into a title package."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PINNED_WINE_COMMIT = "490f6d5dcbb2a5047345b8af88d114bbcaad69a8"


def contained_file(root: Path, relative: str) -> Path:
    candidate = root / relative
    if candidate.is_symlink():
        raise ValueError(f"runtime entry must not be a symlink: {relative}")
    resolved = candidate.resolve(strict=True)
    if root not in resolved.parents or not resolved.is_file():
        raise ValueError(f"runtime entry is not a contained file: {relative}")
    return resolved


def stage(source: Path, destination: Path, manifest_path: Path,
          expect_commit: str) -> int:
    source = source.resolve(strict=True)
    destination = destination.resolve()
    manifest_path = manifest_path.resolve(strict=True)
    if not source.is_dir() or source not in manifest_path.parents:
        raise ValueError("manifest must be inside the Wine runtime directory")
    if source == destination or source in destination.parents or destination in source.parents:
        raise ValueError("source and destination runtime trees must be disjoint")
    if destination.exists():
        raise ValueError(f"refusing to overwrite existing directory: {destination}")

    checked = subprocess.run(
        [sys.executable, str(ROOT / "tools/validate_wine_runtime.py"), "check",
         "--manifest", str(manifest_path), "--distribution", str(source),
         "--expect-commit", expect_commit],
        check=False,
    )
    if checked.returncode != 0:
        return checked.returncode

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    entries = [*manifest["modules"], *manifest.get("data", [])]
    files = [contained_file(source, entry["path"]) for entry in entries]
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".wine-runtime-",
                                     dir=destination.parent) as temporary:
        staged = Path(temporary)
        for entry, source_file in zip(entries, files, strict=True):
            output = staged / entry["path"]
            output.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source_file, output)
        shutil.copyfile(manifest_path,
                        staged / "wine-runtime-manifest.json")
        os.rename(staged, destination)
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--destination", required=True, type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--expect-commit", default=PINNED_WINE_COMMIT)
    arguments = parser.parse_args(argv)
    manifest = arguments.manifest or arguments.source / "wine-runtime-manifest.json"
    try:
        return stage(arguments.source, arguments.destination, manifest,
                     arguments.expect_commit)
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"Wine runtime staging failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
