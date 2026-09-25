#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Build a source-derived Box86 dispatch census and decoder probe matrix."""

from __future__ import annotations

import argparse
from collections import Counter
import json
import re
import subprocess
from pathlib import Path
from typing import Any

BOX86_REPOSITORY = "https://github.com/ptitSeb/box86"
BOX86_COMMIT = "b05cb3ab54a610e09acc594e3aca0419d9ab9e4a"

# Each pair is interpreter dispatch source / ARM DynaRec source. Prefix names
# describe the map selector; they are not a claim that every prefix combination
# or operand form is implemented in either engine.
SOURCE_MAPS: dict[str, tuple[tuple[str, ...], tuple[str, ...]]] = {
    "primary": (("src/emu/x86run.c",), ("src/dynarec/dynarec_arm_00.c",)),
    "0f": (("src/emu/x86run0f.c",), ("src/dynarec/dynarec_arm_0f.c",)),
    "66": (("src/emu/x86run66.c",), ("src/dynarec/dynarec_arm_66.c",)),
    "67": (("src/emu/x86run67.c",), ("src/dynarec/dynarec_arm_67.c",)),
    "segment-64/65": (("src/emu/x86run64.c",),
                      ("src/dynarec/dynarec_arm_64.c", "src/dynarec/dynarec_arm_65.c")),
    "f0": (("src/emu/x86runf0.c",), ("src/dynarec/dynarec_arm_f0.c",)),
    "f20f": (("src/emu/x86runf20f.c",), ("src/dynarec/dynarec_arm_f20f.c",)),
    "f30f": (("src/emu/x86runf30f.c",), ("src/dynarec/dynarec_arm_f30f.c",)),
    "660f": (("src/emu/x86run660f.c",), ("src/dynarec/dynarec_arm_660f.c",)),
    "66f0": (("src/emu/x86runf066.c",), ("src/dynarec/dynarec_arm_66f0.c",)),
    "640f": (("src/emu/x86run640f.c",),
             ("src/dynarec/dynarec_arm_64.c", "src/dynarec/dynarec_arm_0f.c")),
    "6466": (("src/emu/x86run6466.c",),
             ("src/dynarec/dynarec_arm_64.c", "src/dynarec/dynarec_arm_66.c")),
    "6467": (("src/emu/x86run6467.c",),
             ("src/dynarec/dynarec_arm_64.c", "src/dynarec/dynarec_arm_67.c")),
    "6766": (("src/emu/x86run6766.c",),
             ("src/dynarec/dynarec_arm_67.c", "src/dynarec/dynarec_arm_66.c")),
    "66d9": (("src/emu/x86run66d9.c",),
             ("src/dynarec/dynarec_arm_66.c", "src/dynarec/dynarec_arm_d9.c")),
    "66dd": (("src/emu/x86run66dd.c",),
             ("src/dynarec/dynarec_arm_66.c", "src/dynarec/dynarec_arm_dd.c")),
    "66f20f": (("src/emu/x86run66f20f.c",),
               ("src/dynarec/dynarec_arm_66.c", "src/dynarec/dynarec_arm_f20f.c")),
    **{
        f"x87-{opcode:02x}": ((f"src/emu/x86run{opcode:02x}.c",),
                              (f"src/dynarec/dynarec_arm_{opcode:02x}.c",))
        for opcode in range(0xd8, 0xe0)
    },
}
PROBE_MAPS = set(SOURCE_MAPS) | {"0f38", "0f3a"}

CASE_RE = re.compile(r"\bcase\s+(0x[0-9a-fA-F]+|[0-9]+)\s*:")
GO_RE = re.compile(r"\bGO\s*\(\s*0x([0-9a-fA-F]{1,2})\s*,\s*([A-Za-z_]\w*)\s*\)")
GOCOND_RE = re.compile(r"\bGOCOND\s*\(\s*0x([0-9a-fA-F]{1,2})\b")


def strip_comments(text: str) -> str:
    """Remove C comments while retaining line breaks for useful provenance."""
    text = re.sub(r"/\*.*?\*/", lambda match: "\n" * match.group(0).count("\n"),
                  text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def source_case_values(text: str, primary: bool = False) -> dict[int, list[str]]:
    """Return literal case labels plus the two known regular Box86 macros.

    Nested switches (including ModRM selectors) remain visible as raw labels.
    Consumers must treat this as source evidence, not exact ISA support.
    """
    clean = strip_comments(text)
    result: dict[int, list[str]] = {}
    for match in CASE_RE.finditer(clean):
        value = int(match.group(1), 0)
        line = clean.count("\n", 0, match.start()) + 1
        result.setdefault(value, []).append(f"case@{line}")
    if primary:
        for match in GO_RE.finditer(clean):
            base = int(match.group(1), 16)
            line = clean.count("\n", 0, match.start()) + 1
            for delta in range(6):
                result.setdefault(base + delta, []).append(
                    f"GO({match.group(2)})@{line}+{delta}")
        for match in GOCOND_RE.finditer(clean):
            base = int(match.group(1), 16)
            line = clean.count("\n", 0, match.start()) + 1
            for delta in range(16):
                result.setdefault(base + delta, []).append(
                    f"GOCOND@{line}+{delta}")
    return result


def read_git_head(root: Path) -> str:
    return subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                          check=True, capture_output=True, text=True).stdout.strip()


def build_source_inventory(box86_root: Path) -> dict[str, Any]:
    commit = read_git_head(box86_root)
    if commit != BOX86_COMMIT:
        raise ValueError(f"Box86 checkout must be pinned at {BOX86_COMMIT}; found {commit}")
    if not (box86_root / "LICENSE").is_file():
        raise ValueError("Box86 checkout has no root LICENSE")

    rows: dict[tuple[str, int], dict[str, Any]] = {}
    maps: list[dict[str, Any]] = []
    for map_name, (interpreter_paths, arm_paths) in SOURCE_MAPS.items():
        sources = {"interpreter": list(interpreter_paths),
                   "arm_dynarec": list(arm_paths)}
        for engine, relatives in sources.items():
            for relative in relatives:
                path = box86_root / relative
                if not path.is_file():
                    raise ValueError(f"required Box86 source is missing: {relative}")
                cases = source_case_values(path.read_text(encoding="utf-8"),
                                           primary=(map_name == "primary"))
                for opcode, evidence in cases.items():
                    if not 0 <= opcode <= 0xff:
                        continue
                    key = (map_name, opcode)
                    row = rows.setdefault(key, {"map": map_name, "opcode": opcode,
                                                "source_evidence": {}})
                    item = row["source_evidence"].setdefault(
                        engine, {"files": [], "labels": []})
                    if relative not in item["files"]:
                        item["files"].append(relative)
                    item["labels"] = sorted(set(item["labels"]) |
                                             {f"{relative}:{label}" for label in evidence})
        maps.append({"map": map_name, "sources": sources})

    return {
        "repository": BOX86_REPOSITORY,
        "commit": commit,
        "commit_url": f"{BOX86_REPOSITORY}/tree/{commit}",
        "source_url_template": f"{BOX86_REPOSITORY}/blob/{commit}/{{path}}",
        "license": "MIT (root LICENSE; file-level exceptions must be audited)",
        "license_url": f"{BOX86_REPOSITORY}/blob/{commit}/LICENSE",
        "special_license_review": [
            {"file": "src/emu/x86primop.c",
             "note": "contains Realmode X86 Emulator Library notice; do not assume root MIT applies"}
        ],
        "maps": maps,
        "rows": [rows[key] for key in sorted(rows)],
        "interpretation": (
            "Raw source dispatch labels, including nested/group labels and explicit macro "
            "expansions. Presence is a candidate/reference, not proof of correct or complete "
            "instruction support. Interpreter and ARM DynaRec evidence are separate."
        ),
    }


def parse_probe_json(raw: str) -> dict[str, Any]:
    parsed = json.loads(raw)
    if parsed.get("schema") != 1 or parsed.get("probe") != "representative-decode-only":
        raise ValueError("decoder probe schema or mode is not recognized")
    rows = parsed.get("rows")
    if not isinstance(rows, list):
        raise ValueError("decoder probe rows must be an array")
    for row in rows:
        if (row.get("prefix") not in {"none", "66", "67", "f0", "f2", "f3", "64", "65"}
        or row.get("map") not in PROBE_MAPS
                or not isinstance(row.get("opcode"), int)
                or not 0 <= row["opcode"] <= 255
                or not isinstance(row.get("reg_candidate_mask"), int)
                or not isinstance(row.get("mem_candidate_mask"), int)):
            raise ValueError(f"malformed decoder probe row: {row!r}")
    parsed["rows"] = sorted(rows, key=lambda row: (
        row["prefix"], row["map"], row["opcode"]))
    return parsed


def build_catalog(box86_root: Path, probe_binary: Path) -> dict[str, Any]:
    source = build_source_inventory(box86_root)
    probe = subprocess.run([str(probe_binary)], check=True, capture_output=True,
                           text=True, timeout=60)
    probe_data = parse_probe_json(probe.stdout)
    return {
        "schema": "prospero-win-box86-opcode-catalog/1",
        "source": source,
        "prospero": {
            "decoder": "src/pw_x86_block.c: pw_x86_translate",
            "probe": probe_data,
            "probe_note": (
                "A successful representative byte stream proves only that this byte stream "
                "begins with at least one accepted instruction. The masks are sample probes, "
                "not exhaustive encoding-form certification; no emitted code is executed."
            ),
            "execution_tests": [
                "tests/test_pw_x86_block.c",
                "tests/test_pw_x86_engine.c",
                "tests/test_pw_x86_lazyflags.c",
                "tests/test_pw_x86_residency.c",
            ],
            "execution_test_note": (
                "Existing host tests execute selected translated forms and compare with native "
                "references; this artifact does not infer per-form execution coverage from their names."
            ),
        },
    }


def render_markdown(catalog: dict[str, Any]) -> str:
    source = catalog["source"]
    interpreter = sum("interpreter" in row["source_evidence"]
                      for row in source["rows"])
    arm = sum("arm_dynarec" in row["source_evidence"]
              for row in source["rows"])
    probes = catalog["prospero"]["probe"]["rows"]
    probed = len(probes)
    probe_counts = Counter((row["prefix"], row["map"]) for row in probes)
    lines = [
        "# Box86 opcode coverage catalog",
        "",
        "This is a reproducible source-dispatch census plus representative, offline decode probes. "
        "It is a planning aid, not a claim of universal x86 compatibility.",
        "",
        f"- Box86 commit: `{source['commit']}`",
        f"- Box86 license: {source['license']}",
        f"- Box86 dispatch rows with interpreter labels: {interpreter}",
        f"- Rows with ARM DynaRec labels: {arm}",
        f"- Prospero accepted representative probe rows: {probed}",
        "- Prospero decoder probe only calls `pw_x86_translate`; it never executes emitted code.",
        "",
        "## Decode-probe snapshot",
        "",
        "The probe tries bounded byte candidates over the supported prefix/map cross-product. "
        "A mask bit means the first instruction consumed the candidate byte after the opcode; "
        "that byte may be an immediate rather than ModRM. This is a navigation aid only, not "
        "an instruction-form support claim.",
        "",
        "| Prefix | Map | Rows with candidate-byte consumption |",
        "|---|---|---:|",
    ]
    for (prefix, map_name), count in sorted(probe_counts.items()):
        lines.append(f"| `{prefix}` | `{map_name}` | {count} |")
    lines += [
        "",
        "## Primary opcode family bands",
        "",
        "These are navigation bands, not claims that every instruction in a band is implemented.",
        "",
        "| Primary-byte range | Broad family |",
        "|---|---|",
        "| `00–3F` | ALU, compare and accumulator forms |",
        "| `40–5F` | 32-bit INC/DEC and register stack operations |",
        "| `60–6F` | Legacy stack, string and miscellaneous operations |",
        "| `70–7F` | Short conditional branches |",
        "| `80–8F` | Group ALU operations and ModRM data movement |",
        "| `90–9F` | NOP/XCHG and flag operations |",
        "| `A0–AF` | Moffs, string, and TEST operations |",
        "| `B0–BF` | Immediate-to-register moves |",
        "| `C0–CF` | Shifts, returns, interrupts and groups |",
        "| `D0–D7` | Shifts, rotates and XLAT |",
        "| `D8–DF` | x87 escape maps |",
        "| `E0–EF` | Loop, port and control-transfer operations |",
        "| `F0–FF` | Lock/repeat prefixes, flag and grouped operations |",
        "",
        "## Map/source index",
        "",
        "| Map/prefix selector | Box86 interpreter | Box86 ARM DynaRec |",
        "|---|---|---|",
    ]
    for item in source["maps"]:
        interpreter = ", ".join(f"`{path}`" for path in item["sources"]["interpreter"])
        arm = ", ".join(f"`{path}`" for path in item["sources"]["arm_dynarec"])
        lines.append(f"| `{item['map']}` | {interpreter} | {arm} |")
    lines += [
        "",
        "## How to read it",
        "",
        "- A Box86 `case` label is recorded as source evidence only; nested switch labels may "
        "be ModRM group selectors, not opcode bytes. The two engines are listed independently.",
        "- A successful Prospero probe means that one concrete byte stream entered the bounded "
        "decoder. It does not establish every register/memory/width/prefix variant.",
        "- The candidate masks vary the bits normally used by ModRM.reg and choose a "
        "register-shaped or memory-shaped following byte; they are deliberately named candidates because some "
        "non-ModRM instructions consume that byte as an immediate.",
        "- Execution coverage remains owned by the existing host tests; this catalog does not "
        "infer a passing semantic test for every source label.",
        "- Box86's ARM code is not a backend for this project. Reuse the inventory and "
        "semantic/test reference; implement semantics in Prospero's x86-64 emitter.",
        "",
        "## Licensing note",
        "",
        "No Box86 source is vendored by this catalog generator. The root repository is MIT, "
        "but `src/emu/x86primop.c` carries a separate Realmode X86 Emulator Library notice. "
        "Audit the exact file and preserve its notices before copying any implementation. "
        "This repository remains LGPL-2.1-or-later.",
        "",
        "## Regenerate",
        "",
        "```sh",
        "git clone https://github.com/ptitSeb/box86.git /tmp/box86",
        f"git -C /tmp/box86 checkout {source['commit']}",
        "make box86-catalog BOX86_SOURCE=/tmp/box86",
        "```",
        "",
        "The generated JSON is deterministic for the pinned source commit and current Prospero "
        "decoder. Do not update the pin without regenerating, reviewing new file-level notices, "
        "and validating the parser against the changed dispatch layout.",
        "",
    ]
    return "\n".join(lines)


def render_json(catalog: dict[str, Any]) -> str:
    """Pretty-print metadata while keeping each matrix row on one line."""
    lines: list[str] = []

    def emit(value: Any, depth: int, field: str | None = None) -> None:
        indent = "  " * depth
        if isinstance(value, dict):
            lines.append(indent + "{")
            items = list(value.items())
            for index, (key, item) in enumerate(items):
                suffix = "," if index + 1 < len(items) else ""
                label = json.dumps(key) + ": "
                if isinstance(item, list) and all(isinstance(row, dict)
                                                  for row in item):
                    lines.append("  " * (depth + 1) + label + "[")
                    for row_index, row in enumerate(item):
                        row_suffix = "," if row_index + 1 < len(item) else ""
                        compact = json.dumps(row, sort_keys=True,
                                             separators=(",", ":"))
                        lines.append("  " * (depth + 2) + compact + row_suffix)
                    lines.append("  " * (depth + 1) + "]" + suffix)
                elif isinstance(item, dict):
                    lines.append("  " * (depth + 1) + label.rstrip())
                    emit(item, depth + 1)
                    lines[-1] += suffix
                else:
                    lines.append("  " * (depth + 1) + label +
                                 json.dumps(item, sort_keys=True) + suffix)
            lines.append(indent + "}")
        else:
            raise TypeError(f"unexpected nested JSON value for {field}")

    emit(catalog, 0)
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--box86-source", type=Path, required=True,
                        help="Box86 checkout exactly at the pinned commit")
    parser.add_argument("--probe", type=Path, required=True,
                        help="built offline pw_x86_decode_probe helper")
    parser.add_argument("--json-output", type=Path, required=True)
    parser.add_argument("--markdown-output", type=Path, required=True)
    args = parser.parse_args()
    catalog = build_catalog(args.box86_source, args.probe)
    encoded = render_json(catalog)
    args.json_output.parent.mkdir(parents=True, exist_ok=True)
    args.markdown_output.parent.mkdir(parents=True, exist_ok=True)
    args.json_output.write_text(encoded, encoding="utf-8")
    args.markdown_output.write_text(render_markdown(catalog), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
