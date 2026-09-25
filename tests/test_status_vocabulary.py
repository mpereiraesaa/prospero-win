#!/usr/bin/env python3
"""Keep public compatibility status independent of numbered project phases."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
PUBLIC_STATUS_DOCS = (
    ROOT / "README.md",
    ROOT / "docs" / "TECHNICAL_DETAILS.md",
    ROOT / "docs" / "HARDWARE_VALIDATION.md",
)
NUMBERED_STATUS = re.compile(r"\bP[0-9](?:\.[0-9])?\b")


for path in PUBLIC_STATUS_DOCS:
    text = path.read_text(encoding="utf-8")
    match = NUMBERED_STATUS.search(text)
    assert match is None, f"{path}: legacy numbered status {match.group(0)!r}"

readme = (ROOT / "README.md").read_text(encoding="utf-8")
wine = (ROOT / "docs" / "WINE_INTEGRATION.md").read_text(encoding="utf-8")
wine_flat = " ".join(wine.split())
assert "first playable title" in readme
assert "not a project-specific architecture" in readme
assert "copy-and-run" in readme
assert "has now run on the PS5" in readme
assert "The runner still labels process" in readme
assert "explicit DBT marker confirms it reached the `app.exe` entrypoint" in readme
assert "termination `unsupported`" in readme
assert "This proves the generated fixture reached its entrypoint" in readme
assert "Windows apps run. Persistent prefixes" in readme
assert "PW_STAGE_INPUT" in wine_flat
assert "validates the fixture's copy-and-run path" in wine_flat
assert "not a user-installed application" in wine_flat
assert "hardware execution remains an acceptance gate" not in wine_flat
assert "generated-fixture bootstrap has been validated on PS5" in wine_flat
assert ("PS5 execution has only been validated for the generated profile fixture"
        in wine_flat)

print("prospero-win status vocabulary: PASS")
