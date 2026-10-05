#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Reject broken or escaping local Markdown links in public documentation."""

from __future__ import annotations

import re
import sys
from pathlib import Path
from urllib.parse import unquote


ROOT = Path(__file__).resolve().parents[1]
DOCUMENTS = [
    ROOT / "README.md",
    ROOT / "COMPATIBILITY.md",
    ROOT / "CONTRIBUTING.md",
    ROOT / "LICENSING.md",
    ROOT / "NOTICE.md",
    *sorted((ROOT / "docs").glob("*.md")),
]
LINK_RE = re.compile(r"(?<!!)\[[^]]*\]\(([^)]+)\)")


def local_target(raw_target: str) -> str | None:
    target = raw_target.strip()
    if target.startswith("<") and target.endswith(">"):
        target = target[1:-1]
    target = target.split("#", 1)[0]
    if not target or target.startswith(("http://", "https://", "mailto:")):
        return None
    return unquote(target)


def main() -> int:
    failures: list[str] = []
    checked = 0
    root = ROOT.resolve()

    for document in DOCUMENTS:
        text = document.read_text(encoding="utf-8")
        for match in LINK_RE.finditer(text):
            raw_target = match.group(1)
            target = local_target(raw_target)
            if target is None:
                continue
            checked += 1
            resolved = (document.parent / target).resolve()
            try:
                resolved.relative_to(root)
            except ValueError:
                failures.append(
                    f"{document.relative_to(ROOT)}: link escapes repository: {raw_target}"
                )
                continue
            if not resolved.exists():
                failures.append(
                    f"{document.relative_to(ROOT)}: missing link target: {raw_target}"
                )

    if failures:
        print("documentation link audit failed:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1

    print(f"documentation link audit passed: {checked} local links")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
