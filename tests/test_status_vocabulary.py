#!/usr/bin/env python3
"""Keep public compatibility status independent of numbered project phases."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
PUBLIC_STATUS_DOCS = (
    ROOT / "README.md",
    ROOT / "docs" / "ARCHITECTURE.md",
    ROOT / "docs" / "HARDWARE_VALIDATION.md",
)
NUMBERED_STATUS = re.compile(r"\bP[0-9](?:\.[0-9])?\b")


for path in PUBLIC_STATUS_DOCS:
    text = path.read_text(encoding="utf-8")
    match = NUMBERED_STATUS.search(text)
    assert match is None, f"{path}: legacy numbered status {match.group(0)!r}"

readme = " ".join((ROOT / "README.md").read_text(encoding="utf-8").split())
# The public status names what runs on the console today, not a removed path.
assert "runs Wine itself inside a PS5 title" in readme
assert "direct Win32 path has been validated" not in readme

print("prospero-win status vocabulary: PASS")
