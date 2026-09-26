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
readme_flat = " ".join(readme.split())
wine = (ROOT / "docs" / "WINE_INTEGRATION.md").read_text(encoding="utf-8")
wine_flat = " ".join(wine.split())
assert "first playable title" in readme
assert "not a project-specific architecture" in readme
assert "copy-and-run" in readme
assert "On an owned PS5 running firmware 12.02, the direct Win32 path has been validated for" in readme_flat
assert "The separate native Wine bootstrap has reached a generated PE32 fixture's entrypoint on hardware." in readme_flat
assert "That fixture then stopped at an unsupported `NtTerminateThread` service." in readme_flat
assert "This validates the bootstrap path, not general Wine application compatibility." in readme_flat
assert "Persistent prefixes, broader Wine services, and a user-supplied copy-and-run workflow remain in progress." in readme_flat
assert "PE64 applications also need their Windows/native ABI and loader boundaries completed." in readme_flat
assert "PW_STAGE_INPUT" in wine_flat
assert "validates the fixture's copy-and-run path" in wine_flat
assert "not a user-installed application" in wine_flat
assert "hardware execution remains an acceptance gate" not in wine_flat
assert "generated-fixture bootstrap has been validated on PS5" in wine_flat
assert ("PS5 execution has only been validated for the generated profile fixture"
        in wine_flat)

print("prospero-win status vocabulary: PASS")
