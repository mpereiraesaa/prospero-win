#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""The handler registry must not drift from the pinned Wine revision.

The gate's registry is the one place the serviced calls are listed, so this
checks that every id exists in the versioned table carried by
src/pw_unix_call.c, no id appears twice, every entry names a test that exists,
and every class list is unique. tests/test_unix_call_table.py independently
checks that table against the pinned Wine source.
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# One registry entry: id, the class list, the owning test and the handler.
ENTRY = re.compile(
    r'\{\s*(0x[0-9a-f]{4})u,\s*\{\s*([^}]*)\}\s*,\s*"([^"]+)"\s*,\s*'
    r'([A-Za-z_][A-Za-z0-9_]*)\s*\}')
# The versioned Wine call table: id and name.
CALL = re.compile(r'\{\s*(0x[0-9a-f]{4})u,\s*"([^"]+)",\s*(\d+)u\s*\}')
def registry() -> list[tuple[int, list[int], str, str]]:
    text = (ROOT / "src/pw_wine_gate.c").read_text(encoding="utf-8")
    table = text.split("static const PwNtHandler dispatch_table[] = {", 1)[1]
    table = table.split("};", 1)[0]
    entries = []
    for match in ENTRY.finditer(table):
        classes = []
        for token in match.group(2).replace("\n", " ").split(","):
            token = token.strip()
            if not token:
                continue
            if token == "PW_NT_CLASS_NONE":
                break
            assert token.endswith("u"), token
            classes.append(int(token[:-1], 0))
        entries.append((int(match.group(1), 0), classes, match.group(3),
                        match.group(4)))
    return entries


def versioned_calls() -> dict[int, str]:
    text = (ROOT / "src/pw_unix_call.c").read_text(encoding="utf-8")
    return {int(m.group(1), 0): m.group(2) for m in CALL.finditer(text)}


def main() -> int:
    entries = registry()
    calls = versioned_calls()

    assert entries, "the registry is empty"
    assert calls, "the versioned call table is empty"
    ids = [entry[0] for entry in entries]
    assert len(ids) == len(set(ids)), "a call id is registered twice"
    for identifier, classes, test, handler in entries:
        assert identifier in calls, (
            f"0x{identifier:04x} ({handler}) is not in the pinned Wine table")
        assert (ROOT / test).exists(), f"{handler} names a missing test {test}"
        assert len(classes) == len(set(classes)), f"{handler} repeats a class"
        assert handler, "an entry has no handler"

    print("handler ledger passed: "
          f"{len(entries)} serviced calls, all present in the pinned Wine "
          "table with unique ownership and existing tests")
    for identifier, classes, test, handler in entries:
        named = ", ".join(f"0x{class_id:04x}" for class_id in classes)
        print(f"  0x{identifier:04x} {calls[identifier]:<28} "
              f"classes={named or '-'} test={test} handler={handler}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
