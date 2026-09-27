#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Keep the package, runtime telemetry, builder and validator identities in
lockstep.

`PPSA99995` is a local development identifier dedicated to prospero-win, not
a Sony assignment. It must not collide with the identities already installed
on the laboratory console, which this test enforces by rejecting any
reference to them from the packaging, runtime or tooling sources.
"""

from __future__ import annotations

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TITLE_ID = "PPSA99995"
APP_NAME = "prospero-win"
# What the console shows on the home screen; telemetry keeps APP_NAME.
DISPLAY_NAME = "Prospero Win"
CONTENT_ID = "UP9000-PPSA99995_00-PROSPEROWIN00001"
# Split so the publication audit, which forbids these literals anywhere in
# the tree, does not trip over the very test that enforces the separation.
FOREIGN_TITLES = tuple("PPSA999" + suffix for suffix in ("96", "97", "98"))


def require(text: str, marker: str, source: str) -> None:
    if marker not in text:
        raise AssertionError(f"{source} is missing {marker!r}")


def main() -> int:
    param = json.loads((ROOT / "sce_sys/param.json").read_text())
    assert param["titleId"] == TITLE_ID, param["titleId"]
    assert param["conceptId"] == "99995", param["conceptId"]
    assert param["contentId"] == CONTENT_ID, param["contentId"]
    assert param["localizedParameters"]["en-US"]["titleName"] == DISPLAY_NAME

    native = (ROOT / "native/wine64_main.c").read_text()
    require(native, f'#define PW_TITLE_ID "{TITLE_ID}"', "native/wine64_main.c")
    require(native, f'#define PW_APP_NAME "{APP_NAME}-wine64"', "native/wine64_main.c")

    builder = (ROOT / "tools/build_native.sh").read_text()
    require(builder, f"title_id={TITLE_ID}", "tools/build_native.sh")
    require(builder, 'dist="$root/dist/$title_id$output_suffix"',
            "tools/build_native.sh")
    require(builder, 'build="$root/build/native$output_suffix"',
            "tools/build_native.sh")

    validator = (ROOT / "tools/validate_pe_map_evidence.py").read_text()
    require(validator, f'TITLE = "{TITLE_ID}"',
            "tools/validate_pe_map_evidence.py")
    require(validator, f'APP = "{APP_NAME}"',
            "tools/validate_pe_map_evidence.py")

    # A helper pinned to another title would launch the wrong application.
    for source in ("native/wine64_main.c", "tools/build_native.sh",
                   "tools/validate_pe_map_evidence.py",
                   "sce_sys/param.json"):
        text = (ROOT / source).read_text()
        for foreign in FOREIGN_TITLES:
            if foreign in text:
                raise AssertionError(f"{source} references {foreign}")

    print(f"title identity contract passed: {TITLE_ID}/{APP_NAME}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
