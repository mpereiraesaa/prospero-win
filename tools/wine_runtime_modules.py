#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Plan the pinned Wine PE-module closure required by a PE32 image."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

from stage_wine_runtime import PINNED_WINE_COMMIT

ROOT = Path(__file__).resolve().parents[1]
CORE_MODULES = ("ntdll", "kernelbase", "kernel32")
MODULE_RE = re.compile(r"^[a-z0-9_.-]+$")


def module_imports(source: Path, module: str) -> list[str]:
    """Read PE-module dependencies from Wine's Makefile.in IMPORTS field."""
    path = source / "dlls" / module / "Makefile.in"
    if not path.is_file():
        raise ValueError(f"Wine PE module has no Makefile.in: {module}")
    text = path.read_text(errors="strict").replace("\\\n", " ")
    match = re.search(r"^IMPORTS\s*=\s*(.*)$", text, re.M)
    if not match:
        return []
    imports = []
    for token in match.group(1).split("#", 1)[0].split():
        # Makefile variables expand to build-time static libraries, not PE DLLs.
        if "$" in token:
            continue
        name = token.lower()
        if not MODULE_RE.fullmatch(name):
            raise ValueError(f"invalid Wine import module {token!r} in {path}")
        imports.append(name)
    return imports


def resolve_closure(source: Path, roots: list[str]) -> dict:
    """Return the recursive PE-DLL closure, separating Wine import libraries."""
    modules: set[str] = set()
    libraries: set[str] = set()
    edges: dict[str, list[str]] = {}
    visiting: set[str] = set()

    def visit(module: str) -> None:
        name = module.lower().removesuffix(".dll")
        if not MODULE_RE.fullmatch(name):
            raise ValueError(f"invalid Wine module name: {module!r}")
        if name in modules or name in visiting:
            return

        makefile = source / "dlls" / name / "Makefile.in"
        if not makefile.is_file():
            library_makefile = source / "libs" / name / "Makefile.in"
            if library_makefile.is_file():
                libraries.add(name)
                return
            raise ValueError(f"Wine dependency is neither a PE module nor a library: {name}")

        visiting.add(name)
        dependencies = module_imports(source, name)
        pe_dependencies = []
        for dependency in dependencies:
            dep_makefile = source / "dlls" / dependency / "Makefile.in"
            if dep_makefile.is_file():
                pe_dependencies.append(dependency)
                visit(dependency)
            elif (source / "libs" / dependency / "Makefile.in").is_file():
                libraries.add(dependency)
            else:
                raise ValueError(
                    f"{name} imports unknown Wine dependency {dependency}")
        visiting.remove(name)
        modules.add(name)
        edges[name] = sorted(set(pe_dependencies))

    for root in sorted(set(roots) | set(CORE_MODULES)):
        visit(root)

    return {
        "modules": sorted(modules),
        "library_imports": sorted(libraries),
        "dependencies": {name: edges[name] for name in sorted(edges)},
    }


def inspect_image(image: Path) -> dict:
    inspector = ROOT / "build/host/inspect_pe"
    if not inspector.is_file():
        raise RuntimeError("build the canonical PE inspector first: make build/host/inspect_pe")
    result = subprocess.run([str(inspector), str(image), "--imports-json"],
                            check=True, capture_output=True, text=True)
    data = json.loads(result.stdout)
    if data.get("schema") != "pw-imports/1":
        raise ValueError("unexpected PE inspector schema")
    if data.get("machine") != 332:
        raise ValueError("the pinned runtime planner currently accepts i386 PE32 only")
    return data


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="PE32 application image")
    parser.add_argument("--wine-source", type=Path, required=True,
                        help="clean checkout of the pinned Wine source")
    parser.add_argument("--expect-commit", default=PINNED_WINE_COMMIT,
                        help="required Wine source commit (default: project pin)")
    parser.add_argument("--output", type=Path,
                        help="new JSON output path; refuses to overwrite")
    args = parser.parse_args()

    source = args.wine_source.resolve(strict=True)
    image = args.image.resolve(strict=True)
    commit = subprocess.run(["git", "-C", str(source), "rev-parse", "HEAD"],
                            check=True, capture_output=True, text=True).stdout.strip()
    if commit != args.expect_commit:
        raise ValueError(f"Wine source is {commit}, expected {args.expect_commit}")
    dirty = subprocess.run(["git", "-C", str(source), "status", "--porcelain"],
                           check=True, capture_output=True, text=True).stdout.strip()
    if dirty:
        raise ValueError("Wine source checkout must be clean")

    image_digest = hashlib.sha256(image.read_bytes()).hexdigest()
    pe = inspect_image(image)
    if hashlib.sha256(image.read_bytes()).hexdigest() != image_digest:
        raise RuntimeError("PE image changed during inspection")
    roots = sorted({module["dll"].lower().removesuffix(".dll")
                    for module in pe["modules"]})
    closure = resolve_closure(source, roots)
    report = {
        "schema": "pw-wine-runtime-modules/1",
        "wine_commit": commit,
        "image_sha256": image_digest,
        "machine": pe["machine"],
        "roots": roots,
        "root_import_count": sum(len(module["imports"]) for module in pe["modules"]),
        "scope": "static PE imports plus Wine Makefile.in PE-module IMPORTS closure",
        "dynamic_imports": "unknown until runtime or further analysis",
        **closure,
    }
    text = json.dumps(report, indent=2) + "\n"
    if args.output:
        with args.output.open("x", encoding="utf-8") as handle:
            handle.write(text)
    else:
        print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
