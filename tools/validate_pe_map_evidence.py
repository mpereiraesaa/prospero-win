#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Fail-closed validation for the prospero-win PE mapping gate.

Consumes a ps5logd manifest plus its transcript and accepts the run only
when the transport, the identity and every PW_* record agree. The load order
is re-derived here from the PW_DEP edges rather than trusted from the
runtime's own PW_ORDER conclusion: the point of the record set is that an
independent checker can contradict the program that produced it.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path

TITLE = "PPSA99995"
APP = "prospero-win"
SCHEMA = "1"
SLICE = "pe-map"
HEX64 = re.compile(r"^[0-9a-f]{64}$")


class EvidenceError(RuntimeError):
    pass


def fail(message: str) -> None:
    raise EvidenceError(message)


def parse_fields(message: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in message.split()[1:]:
        if "=" not in token:
            fail(f"malformed record field: {token}")
        key, value = token.split("=", 1)
        if key in fields:
            fail(f"duplicate record field: {key}")
        fields[key] = value
    return fields


def as_int(fields: dict[str, str], key: str) -> int:
    try:
        return int(fields[key], 0)
    except (KeyError, ValueError):
        fail(f"invalid integer field: {key}")
        raise AssertionError


def as_optional_int(fields: dict[str, str], key: str, default: int) -> int:
    """Read an additive evidence field without invalidating older captures."""
    if key not in fields:
        return default
    return as_int(fields, key)


def require(fields: dict[str, str], key: str, expected: str) -> None:
    if fields.get(key) != expected:
        fail(f"expected {key}={expected}, found {key}={fields.get(key)}")


def one(records: list[str], prefix: str) -> dict[str, str]:
    matches = [record for record in records if record.startswith(prefix + " ")]
    if len(matches) != 1:
        fail(f"expected exactly one {prefix}, found {len(matches)}")
    return parse_fields(matches[0])


def many(records: list[str], prefix: str) -> list[dict[str, str]]:
    return [parse_fields(record) for record in records
            if record.startswith(prefix + " ")]


def read_transcript(manifest_path: Path, manifest: dict[str, object]) -> list[str]:
    log_name = manifest.get("log_path")
    if not isinstance(log_name, str) or Path(log_name).name != log_name:
        fail("unsafe transcript path")
    log_path = (manifest_path.parent / log_name).resolve()
    if log_path.parent != manifest_path.parent:
        fail("transcript escaped the manifest directory")
    try:
        data = log_path.read_bytes()
    except OSError as error:
        fail(f"missing transcript: {error}")
    if len(data) != manifest.get("bytes"):
        fail("transcript size mismatch")
    digest = hashlib.sha256(data).hexdigest()
    if not HEX64.match(str(manifest.get("sha256", ""))) or \
            digest != manifest.get("sha256"):
        fail("transcript hash mismatch")
    return data.decode("utf-8").splitlines()


def check_transport(manifest: dict[str, object], lines: list[str]) -> list[str]:
    identity = manifest.get("identity")
    if not isinstance(identity, dict):
        fail("manifest has no identity")
    if identity.get("title") != TITLE or identity.get("app") != APP:
        fail(f"identity mismatch: {identity.get('title')}/{identity.get('app')}")
    if manifest.get("protocol") != "ps5log/1" or manifest.get("transport") != "tcp":
        fail("protocol or transport mismatch")
    if not all(manifest.get(key) for key in ("hello", "bye", "clean")):
        fail("run lacks a clean HELLO/BYE completion")
    if manifest.get("gaps") != [] or manifest.get("raw_lines") != 0 or \
            manifest.get("oversized_lines") != 0:
        fail("run contains transport corruption")

    if not lines or not lines[0].startswith("HELLO ps5log/1 "):
        fail("HELLO line missing")
    hello = parse_fields("HELLO " + lines[0].split(" ", 2)[2])
    require(hello, "title", TITLE)
    require(hello, "app", APP)
    if hello.get("boot") != str(identity.get("boot", "")):
        fail("HELLO boot token does not match the manifest")

    records: list[str] = []
    expected_seq = 1
    last_seq = 0
    for line in lines[1:]:
        if line.startswith("BYE "):
            bye = parse_fields(line)
            if as_int(bye, "seq") != last_seq:
                fail("BYE sequence does not match the last record")
            return records
        parts = line.split("\t")
        if len(parts) != 4:
            fail(f"unstructured line in transcript: {line[:64]}")
        seq = int(parts[0])
        if seq != expected_seq:
            fail(f"sequence gap: expected {expected_seq}, found {seq}")
        if parts[2] == "ERR":
            fail(f"run reported an error record: {parts[3][:96]}")
        expected_seq += 1
        last_seq = seq
        records.append(parts[3])
    fail("BYE record missing")
    raise AssertionError


def check_modules(records: list[str], *, allow_i386: bool,
                  allow_wx: bool) -> dict[str, object]:
    modules = many(records, "PW_MODULE")
    if not modules:
        fail("no PW_MODULE record")
    graph = one(records, "PW_GRAPH")
    by_index: dict[int, dict[str, str]] = {}
    mapped_indices: set[int] = set()
    machines: set[str] = set()
    kind_counts = {"root": 0, "local": 0, "runtime": 0, "host": 0}

    for module in modules:
        index = as_int(module, "index")
        if index in by_index:
            fail(f"module index {index} reported twice")
        by_index[index] = module
    if sorted(by_index) != list(range(len(by_index))):
        fail("module indices are not a dense range from zero")
    if as_int(graph, "modules") != len(by_index):
        fail("PW_GRAPH module count disagrees with the PW_MODULE records")

    for index, module in sorted(by_index.items()):
        kind = module.get("kind")
        if kind not in ("root", "local", "runtime", "host"):
            fail(f"module {index} has an unknown kind: {kind}")
        kind_counts[kind] += 1
        is_mapped = as_int(module, "mapped") == 1

        if kind == "host":
            # A Win32 module is an interface, never bytes read from disk.
            if is_mapped:
                fail(f"host module {module.get('name')} was mapped from disk")
            require(module, "base", "0x0")
            require(module, "machine", "none")
            continue

        if not is_mapped:
            fail(f"module {module.get('name')} was never mapped")
        mapped_indices.add(index)
        machines.add(str(module.get("machine")))
        require(module, "headers", "1")
        for key in ("verify_mismatch", "verify_zero_tail", "verify_alias"):
            if as_int(module, key) != 0:
                fail(f"module {module.get('name')} failed {key}")
        if as_int(module, "verify_sections") != as_int(module, "sections"):
            fail(f"module {module.get('name')} verified a partial section set")
        if as_int(module, "image_bytes") == 0:
            fail(f"module {module.get('name')} mapped zero bytes")
        base = as_int(module, "base")
        preferred = as_int(module, "preferred")
        if base == 0:
            fail(f"module {module.get('name')} has no load address")
        if base != preferred and as_int(module, "reloc_applied") == 0:
            # Rebased without a single applied relocation: the image would
            # be holding pointers to an address it is not loaded at.
            fail(f"module {module.get('name')} was rebased without relocation")
        if base == preferred and as_int(module, "reloc_applied") != 0:
            fail(f"module {module.get('name')} relocated at its own base")
        if as_int(module, "native") != 1 and not allow_i386:
            fail(f"module {module.get('name')} is not natively executable; "
                 "pass --allow-i386 to accept a parse-only run")

    if kind_counts["root"] != 1 or by_index[0].get("kind") != "root":
        fail("the graph must contain exactly one root at module index zero")
    for kind in ("local", "runtime", "host"):
        # `runtime` was added after the first accepted hardware captures.
        # Its absence in those immutable transcripts means zero; a present
        # value remains strict and must agree with PW_MODULE records.
        reported = as_optional_int(graph, kind, 0) if kind == "runtime" \
            else as_int(graph, kind)
        if reported != kind_counts[kind]:
            fail(f"PW_GRAPH {kind} count disagrees with PW_MODULE records")

    if len(machines) > 1:
        fail(f"the graph mixes instruction sets: {sorted(machines)}")
    if machines and graph.get("machine") not in machines:
        fail("PW_GRAPH machine disagrees with the mapped modules")

    protections = many(records, "PW_PROTECT")
    protected = {as_int(record, "index") for record in protections}
    if protected != mapped_indices:
        fail("PW_PROTECT does not cover exactly the mapped modules")
    for record in protections:
        require(record, "applied", "1")
        if as_int(record, "pages") == 0 or as_int(record, "calls") == 0:
            fail(f"module {record.get('name')} protected no pages")
        if as_int(record, "wx") != 0 and not allow_wx:
            # A page both writable and executable is a real weakening, so it
            # has to be acknowledged explicitly rather than discovered later.
            fail(f"module {record.get('name')} left "
                 f"{as_int(record, 'wx')} writable-executable pages; "
                 "pass --allow-wx to accept a coarse mapping granularity")
    return {"by_index": by_index, "graph": graph, "mapped": mapped_indices}


def check_compat32(records: list[str], expectation: str) -> dict[str, object]:
    """Validate the compatibility-mode record without prejudging its answer.

    Whether this firmware allows 32-bit compatibility mode is the thing
    being measured, so a refusal is a valid result. What is checked is that
    the record is internally consistent and cannot claim more than it
    demonstrated; the operator asserts the expected outcome explicitly.
    """
    found = many(records, "PW_COMPAT32")
    if not found:
        if expectation != "any":
            fail(f"expected compat32={expectation} but no PW_COMPAT32 record")
        return {"compat32": "absent"}
    if len(found) != 1:
        fail(f"expected exactly one PW_COMPAT32, found {len(found)}")
    record = found[0]
    require(record, "schema", SCHEMA)

    install = record.get("install")
    proven = as_int(record, "proven")
    attempted = as_int(record, "attempted")
    returned = as_int(record, "returned")

    if install != "ok":
        # Descriptor installation was refused: nothing may have been run.
        if attempted != 0 or returned != 0 or proven != 0:
            fail("compat32 claims a transfer after installation failed")
        outcome = "refused"
    elif attempted == 0:
        if proven != 0:
            fail("compat32 claims proof without attempting the transfer")
        outcome = "installed"
    elif returned == 0:
        if proven != 0:
            fail("compat32 claims proof without returning")
        outcome = "entered-no-return"
    else:
        expected = as_int(record, "expected")
        result = as_int(record, "result")
        genuine = result == expected and \
            record.get("cs_seen") == record.get("code_sel")
        if bool(proven) != genuine:
            fail("compat32 proven flag disagrees with its own result and "
                 "selector")
        if proven:
            for key in ("reserve", "build", "seal", "transfer"):
                if record.get(key) != "ok":
                    fail(f"compat32 proven but {key}={record.get(key)}")
        outcome = "proven" if proven else "returned-wrong-result"

    if expectation == "proven" and outcome != "proven":
        fail(f"expected compat32 proven, observed {outcome}")
    if expectation == "refused" and outcome != "refused":
        fail(f"expected compat32 refused, observed {outcome}")
    return {
        "compat32": outcome,
        "compat32_install": install,
        "compat32_result": record.get("result"),
        "compat32_cs_seen": record.get("cs_seen"),
    }


def check_order(records: list[str], by_index: dict[int, dict[str, str]],
                graph: dict[str, str]) -> None:
    order = many(records, "PW_ORDER")
    if as_int(graph, "ordered") != len(order):
        fail("PW_GRAPH order count disagrees with the PW_ORDER records")
    positions: dict[int, int] = {}
    for record in order:
        position = as_int(record, "position")
        index = as_int(record, "index")
        if position != len(positions):
            fail(f"PW_ORDER position {position} is out of sequence")
        if index in positions:
            fail(f"module {index} appears twice in the load order")
        if index not in by_index:
            fail(f"PW_ORDER names unknown module {index}")
        if record.get("name") != by_index[index].get("name"):
            fail(f"PW_ORDER name disagrees with PW_MODULE for index {index}")
        positions[index] = position
    if sorted(positions) != sorted(by_index):
        fail("the load order is not a permutation of the module set")

    edges = many(records, "PW_DEP")
    declared: dict[int, int] = {index: 0 for index in by_index}
    for edge in edges:
        index = as_int(edge, "index")
        dependency = as_int(edge, "dep_index")
        if index not in by_index or dependency not in by_index:
            fail("PW_DEP names an unknown module")
        if edge.get("name") != by_index[index].get("name") or \
                edge.get("dep_name") != by_index[dependency].get("name"):
            fail("PW_DEP names disagree with the PW_MODULE records")
        if edge.get("dep_kind") != by_index[dependency].get("kind"):
            fail("PW_DEP kind disagrees with the target PW_MODULE")
        if index == dependency:
            fail(f"module {edge.get('name')} depends on itself")
        declared[index] += 1
        # Dependencies must be initialisable before their dependents. A
        # mutual import is legal in PE, and PW_GRAPH must own up to it.
        if positions[dependency] > positions[index] and \
                as_int(graph, "cycles") == 0:
            fail(f"{edge.get('dep_name')} is loaded after "
                 f"{edge.get('name')} but no cycle was reported")
    for index, module in by_index.items():
        if declared[index] != as_int(module, "deps"):
            fail(f"module {module.get('name')} declared "
                 f"{as_int(module, 'deps')} dependencies but emitted "
                 f"{declared[index]} edges")


def check_call6(records: list[str], required: bool = False) -> bool:
    if not many(records, "PW_CALL6") and not required:
        return False
    call = one(records, "PW_CALL6")
    require(call, "kind", "synthetic-code")
    require(call, "status", "ok")
    for key, expected in {"constant": 42, "alignment": 8, "weighted": 278,
                          "high": 4294967574, "sealed": 1,
                          "released": 1}.items():
        if as_int(call, key) != expected:
            fail(f"PW_CALL6 {key} differs from expected {expected}")
    return True


def validate(manifest_path: Path, *, root: str | None, expect_modules: int | None,
             expect_local: int | None, expect_runtime: int | None,
             expect_host: int | None,
             allow_i386: bool, allow_wx: bool,
             expect_compat32: str = "any", expect_call6: bool = False) -> dict[str, object]:
    manifest_path = manifest_path.resolve()
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(f"invalid manifest: {error}")
    if not isinstance(manifest, dict):
        fail("manifest is not an object")

    lines = read_transcript(manifest_path, manifest)
    records = check_transport(manifest, lines)
    call6 = check_call6(records, expect_call6)

    boot = one(records, "PW_BOOT")
    require(boot, "schema", SCHEMA)
    require(boot, "slice", SLICE)
    if root is not None and boot.get("root") != root:
        fail(f"expected root={root}, found root={boot.get('root')}")
    if as_int(boot, "root_bytes") == 0:
        fail("PW_BOOT reports an empty root image")
    if as_int(boot, "page_bytes") == 0:
        fail("PW_BOOT reports no protection granularity")

    compat32 = check_compat32(records, expect_compat32)

    checked = check_modules(records, allow_i386=allow_i386, allow_wx=allow_wx)
    by_index: dict[int, dict[str, str]] = checked["by_index"]  # type: ignore[assignment]
    graph: dict[str, str] = checked["graph"]                   # type: ignore[assignment]
    mapped: set[int] = checked["mapped"]                       # type: ignore[assignment]
    check_order(records, by_index, graph)

    exit_record = one(records, "PW_EXIT")
    require(exit_record, "result", "0")
    require(exit_record, "status", "ok")
    require(exit_record, "missing", "none")
    require(exit_record, "truncated", "0")
    if as_int(exit_record, "modules") != len(by_index):
        fail("PW_EXIT module count disagrees with the PW_MODULE records")
    if as_int(exit_record, "mapped") != len(mapped):
        fail("PW_EXIT mapped count disagrees with the PW_MODULE records")
    if as_int(exit_record, "released") != as_int(exit_record, "mapped"):
        fail("not every mapped image was released")

    counts = {
        "modules": as_int(graph, "modules"),
        "local": as_int(graph, "local"),
        "runtime": as_optional_int(graph, "runtime", 0),
        "host": as_int(graph, "host"),
    }
    for key, expected in (("modules", expect_modules), ("local", expect_local),
                          ("runtime", expect_runtime),
                          ("host", expect_host)):
        if expected is not None and counts[key] != expected:
            fail(f"expected {key}={expected}, found {key}={counts[key]}")

    summary: dict[str, object] = {
        "title": TITLE,
        "app": APP,
        "root": boot.get("root"),
        "machine": graph.get("machine"),
        "modules": counts["modules"],
        "local": counts["local"],
        "runtime": counts["runtime"],
        "host": counts["host"],
        "mapped": len(mapped),
        "cycles": as_int(graph, "cycles"),
        "max_depth": as_int(graph, "max_depth"),
        "reserved_bytes": as_int(graph, "reserved_bytes"),
        "records": len(records),
        "sha256": manifest.get("sha256"),
    }
    summary.update(compat32)
    summary["call6"] = "passed" if call6 else "not-tested"
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--root", help="expected root module name")
    parser.add_argument("--expect-call6", action="store_true",
                        help="require synthetic Win64 integer execution proof")
    parser.add_argument("--expect-modules", type=int)
    parser.add_argument("--expect-local", type=int)
    parser.add_argument("--expect-runtime", type=int)
    parser.add_argument("--expect-host", type=int)
    parser.add_argument("--allow-i386", action="store_true",
                        help="accept a parse-only run of a 32-bit image")
    parser.add_argument("--allow-wx", action="store_true",
                        help="accept writable-executable pages forced by a "
                             "coarse mapping granularity")
    parser.add_argument("--expect-compat32", default="any",
                        choices=("any", "proven", "refused"),
                        help="assert the compatibility-mode outcome")
    arguments = parser.parse_args()

    try:
        summary = validate(arguments.manifest, root=arguments.root,
                           expect_modules=arguments.expect_modules,
                           expect_local=arguments.expect_local,
                           expect_runtime=arguments.expect_runtime,
                           expect_host=arguments.expect_host,
                           allow_i386=arguments.allow_i386,
                           allow_wx=arguments.allow_wx,
                           expect_compat32=arguments.expect_compat32,
                           expect_call6=arguments.expect_call6)
    except EvidenceError as error:
        print(f"pe-map evidence rejected: {error}")
        return 1
    print("pe-map evidence accepted")
    for key, value in summary.items():
        print(f"  {key}={value}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
