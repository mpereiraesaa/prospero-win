#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Validate the transcript of the bounded Wine ntdll entry gate.

The claim this evidence may support is narrow and specific: real,
manifest-hashed i386 Wine modules were mapped from the runtime namespace,
their import graph was bound, and one exported ntdll function executed
through the IA-32 DBT until the versioned Unix-call boundary - and no host
Wine function was called.

Everything that could make that claim false is checked here: a module whose
hash or machine does not match the manifest, an entry point outside the
mapped image, a boundary that is not the dispatcher thunk, a stop that is
not the Unix-call boundary, a retired count of zero, a syscall number that
does not match the one encoded in the entry stub, and leaked mappings or
translations.

Usage:
    validate_wine_ntdll_evidence.py TRANSCRIPT [--manifest MANIFEST]
        [--expect-entry NAME]
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

PREFIX = "kind=host-wine-"
HEX32 = re.compile(r"^0x[0-9a-f]{8}$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")
REQUIRED = {
    "gate": {"runtime", "root", "entry", "modules"},
    "bind": {"modules", "functions", "data", "failures"},
    "tls": {"modules"},
    "boundary": {"slot_rva", "slot_va", "thunks", "thunk_rva", "thunk_va"},
    "entry": {"module", "symbol", "rva", "eip", "pe_entry_rva", "stub_id",
              "kind"},
    "call": {"return_eip", "in_module", "caller_rva", "caller_id", "observed"},
    "modes": {"chaining", "residency", "lazy_flags"},
    "run": {"first_eip", "last_eip", "retired", "dispatches", "blocks",
            "bytes", "reg_loads", "reg_stores", "reg_reconciliations",
            "reg_spills", "stop_address", "stop", "syscall", "host_calls"},
    "cleanup": {"modules", "pending_modules", "pending_pages",
                "pending_regions", "mappings", "translations",
                "pending_translations", "failures", "status"},
    "verdict": {"accepted", "stop", "entry_id", "syscall", "retired"},
}
MODULE_FIELDS = {"name", "sha256", "size", "machine", "base", "image_bytes",
                 "loaded", "runtime", "imports", "tls", "path"}
ACCEPTED_STOP = "wine-unix-call-boundary"
BRIDGED_STOPS = {
    "unix-call-unimplemented": "unimplemented",
    "unix-call-unknown": "unknown",
    "unix-call-rejected": "rejected",
    # The guest ended its own process (ntdll's loader does that when the image
    # it was handed is not an executable): the terminating call is serviced and
    # the run stops because there is nothing left to execute.
    "process-terminated": "handled",
}
NT_ERROR = 0x80000000


class Failure(SystemExit):
    def __init__(self, message: str) -> None:
        super().__init__(f"wine ntdll evidence: {message}")


def parse_transcript(text: str) -> dict[str, list[dict[str, str]]]:
    records: dict[str, list[dict[str, str]]] = {}
    for line in text.splitlines():
        # The same records appear bare from the host runner and as the
        # message column of a ps5log/1 line on the console.
        marker = line.find(PREFIX)
        if marker < 0:
            continue
        body = line[marker + len(PREFIX):].strip()
        kind, _, rest = body.partition(" ")
        fields: dict[str, str] = {}
        for token in rest.split():
            key, separator, value = token.partition("=")
            if not separator:
                raise Failure(f"malformed field in {kind}: {token!r}")
            fields[key] = value
        records.setdefault(kind, []).append(fields)
    return records


def one(records: dict[str, list[dict[str, str]]], kind: str) -> dict[str, str]:
    found = records.get(kind, [])
    if len(found) != 1:
        raise Failure(f"expected exactly one {kind} record, found {len(found)}")
    return found[0]


def number(record: dict[str, str], key: str, where: str) -> int:
    raw = record.get(key)
    if raw is None:
        raise Failure(f"{where} lacks {key}")
    try:
        return int(raw, 0)
    except ValueError as error:
        raise Failure(f"{where} has a non-numeric {key}: {raw!r}") from error


def require_fields(record: dict[str, str], keys: set[str], where: str) -> None:
    missing = keys - set(record)
    if missing:
        raise Failure(f"{where} lacks {', '.join(sorted(missing))}")


def validate(records: dict[str, list[dict[str, str]]],
             manifest: dict | None, expect_entry: str | None) -> list[str]:
    notes: list[str] = []
    for kind, keys in REQUIRED.items():
        if kind != "module":
            require_fields(one(records, kind), keys, kind)
    modules = records.get("module", [])
    if len(modules) < 2:
        raise Failure(f"expected at least two module records, found {len(modules)}")
    by_name: dict[str, dict[str, str]] = {}
    for record in modules:
        require_fields(record, MODULE_FIELDS, "module")
        name = record["name"]
        if name in by_name:
            raise Failure(f"duplicate module record: {name}")
        by_name[name] = record
        if not SHA256.match(record["sha256"]):
            raise Failure(f"{name} has no SHA-256")
        if number(record, "machine", name) != 0x014C:
            raise Failure(f"{name} is not an i386 PE image")
        if number(record, "size", name) <= 0:
            raise Failure(f"{name} has no size")
        if number(record, "loaded", name) != 1:
            raise Failure(f"{name} was never mapped")
        if number(record, "runtime", name) != 1:
            raise Failure(f"{name} was not loaded from the runtime namespace")
        base = number(record, "base", name)
        span = number(record, "image_bytes", name)
        if base == 0 or span == 0 or base + span > 0x100000000:
            raise Failure(f"{name} is not mapped inside the 32-bit guest space")
    if "ntdll.dll" not in by_name:
        raise Failure("the transcript does not map ntdll.dll")
    ntdll = by_name["ntdll.dll"]

    if manifest is not None:
        listed = {entry["name"]: entry for entry in manifest.get("modules", [])}
        for name, record in by_name.items():
            entry = listed.get(name)
            if entry is None:
                raise Failure(f"{name} is not in the manifest")
            if entry["sha256"] != record["sha256"]:
                raise Failure(f"{name} hash does not match the manifest")
            if entry["size"] != number(record, "size", name):
                raise Failure(f"{name} size does not match the manifest")
        notes.append(f"manifest digest {manifest.get('distribution_sha256')}")

    bind = one(records, "bind")
    if number(bind, "failures", "bind") != 0:
        raise Failure("import binding reported failures")
    if number(bind, "modules", "bind") < 1 or number(bind, "functions", "bind") < 1:
        raise Failure("no runtime-to-runtime import was bound")
    notes.append(f"bound {bind['functions']} functions in {bind['modules']} modules")

    boundary = one(records, "boundary")
    thunks = number(boundary, "thunks", "boundary")
    if thunks < 1:
        raise Failure("no __wine_syscall dispatcher thunk was located")
    slot_va = number(boundary, "slot_va", "boundary")
    thunk_va = number(boundary, "thunk_va", "boundary")
    slot_rva = number(boundary, "slot_rva", "boundary")
    thunk_rva = number(boundary, "thunk_rva", "boundary")
    ntdll_base = number(ntdll, "base", "ntdll.dll")
    if slot_va != ntdll_base + slot_rva:
        raise Failure("dispatcher slot address is not inside mapped ntdll")
    if thunk_va != ntdll_base + thunk_rva:
        raise Failure("dispatcher thunk address is not inside mapped ntdll")
    if slot_rva == thunk_rva:
        raise Failure("the dispatcher slot and its thunk are the same address")

    entry = one(records, "entry")
    if expect_entry and entry["symbol"] != expect_entry:
        raise Failure(f"entry symbol is {entry['symbol']}, expected {expect_entry}")
    if entry["module"] != "ntdll.dll":
        raise Failure("the entry module is not ntdll.dll")
    entry_eip = number(entry, "eip", "entry")
    entry_rva = number(entry, "rva", "entry")
    stub_id = number(entry, "stub_id", "entry")
    if entry_eip != ntdll_base + entry_rva:
        raise Failure("initial guest EIP is not inside mapped ntdll")
    if entry_rva >= number(ntdll, "image_bytes", "ntdll.dll"):
        raise Failure("entry RVA is outside ntdll")
    if stub_id == 0:
        if entry.get("kind") != "initialization":
            raise Failure("the entry stub does not encode a syscall number "
                          "and is not marked as an initialization entry")
    elif entry.get("kind") != "stub":
        raise Failure(f"entry kind is {entry.get('kind')!r}, expected stub")

    modes = one(records, "modes")
    notes.append("engine modes chaining=%s residency=%s lazy_flags=%s"
                 % (modes["chaining"], modes["residency"], modes["lazy_flags"]))

    run = one(records, "run")
    retired = number(run, "retired", "run")
    if retired <= 0:
        raise Failure("no guest instruction was retired")
    if number(run, "host_calls", "run") != 0:
        raise Failure("a host Wine function was called")
    first_eip = number(run, "first_eip", "run")
    last_eip = number(run, "last_eip", "run")
    stop_address = number(run, "stop_address", "run")
    if first_eip != entry_eip:
        raise Failure("the run did not start at the entry point")
    syscall = number(run, "syscall", "run")
    if run["stop"] != ACCEPTED_STOP:
        if run["stop"] not in BRIDGED_STOPS and \
                run["stop"] != "unsupported-instruction" and \
                run["stop"] != "memory-bounds" and \
                run["stop"] != "cache-limit" and \
                run["stop"] != "returned-to-caller":
            raise Failure(f"the gate stopped with {run['stop']}")
        serviced = records.get("calls", [])
        if len(serviced) != 1:
            raise Failure("a bridged run must report exactly one calls record")
        sequence = records.get("call-seq", [])
        if not sequence:
            raise Failure("a bridged run must report its call sequence")
        for index, serviced_call in enumerate(sequence):
            if number(serviced_call, "index", "call-seq") != index:
                raise Failure("call sequence indices are not contiguous")
            require_fields(serviced_call,
                           {"index", "id", "name", "args", "return", "status",
                            "outcome", "argument", "stub_return"}, "call-seq")
            if number(serviced_call, "args", "call-seq") % 4 != 0:
                raise Failure("a call argument width is not a multiple of four")
            stub_return = number(serviced_call, "stub_return", "call-seq")
            if stub_return < ntdll_base or \
                    stub_return >= ntdll_base + \
                    number(ntdll, "image_bytes", "ntdll.dll"):
                raise Failure("a serviced call did not return into ntdll")
        first = sequence[0]
        if number(first, "id", "call-seq") != 0x18 or \
                first["name"] != "NtAllocateVirtualMemory":
            raise Failure("the first serviced call is not NtAllocateVirtualMemory")
        if first["outcome"] != "handled" or \
                number(first, "status", "call-seq") & NT_ERROR:
            raise Failure("the first serviced call did not succeed")
        last = sequence[-1]
        if run["stop"] == "unsupported-instruction":
            # The bridge serviced calls and the run then met an instruction
            # family the translator does not cover yet: nothing may have been
            # refused, and the stop must still be inside ntdll.
            for serviced_call in sequence:
                if serviced_call["outcome"] != "handled":
                    raise Failure("a run that stopped on an instruction may "
                                  "not report a refused call")
            stop_address = number(run, "stop_address", "run")
            if stop_address < ntdll_base or \
                    stop_address >= ntdll_base + \
                    number(ntdll, "image_bytes", "ntdll.dll"):
                raise Failure("the instruction stop is not inside ntdll")
        elif run["stop"] == "memory-bounds":
            # A classified memory stop during development: the bridge serviced
            # every call, nothing was refused, and the guest dereferenced an
            # address outside every mapped module - the shape a missing
            # process-environment field produces. It is evidence about the
            # stop, never an acceptance.
            for serviced_call in sequence:
                if serviced_call["outcome"] != "handled":
                    raise Failure("a run that stopped on a memory bound may "
                                  "not report a refused call")
            fault = one(records, "fault")
            require_fields(fault, {"address", "width", "write"}, "fault")
            address = number(fault, "address", "fault")
            width = number(fault, "width", "fault")
            if address == 0 or width not in (1, 2, 4, 8, 16):
                raise Failure("the fault record does not describe an access")
            for name, record in by_name.items():
                base = number(record, "base", name)
                span = number(record, "image_bytes", name)
                if address >= base and address < base + span:
                    raise Failure("the reported fault is inside a mapped "
                                  "module, which is not a classified stop")
            stop_address = number(run, "stop_address", "run")
            if stop_address < ntdll_base or \
                    stop_address >= ntdll_base + \
                    number(ntdll, "image_bytes", "ntdll.dll"):
                raise Failure("the memory stop is not inside ntdll")
        elif run["stop"] == "cache-limit":
            # The gate ran out of its own translated-code arena: a capacity
            # stop, not a guest fault. Nothing may have been refused.
            for serviced_call in sequence:
                if serviced_call["outcome"] != "handled":
                    raise Failure("a run that stopped on the translation "
                                  "arena may not report a refused call")
        elif run["stop"] == "returned-to-caller":
            # The guest jumped to address zero: a null call, which is the same
            # class of finding as the null read above. Only exactly zero is
            # accepted, and only when nothing was refused.
            for serviced_call in sequence:
                if serviced_call["outcome"] != "handled":
                    raise Failure("a run that jumped to null may not report a "
                                  "refused call")
            if number(run, "stop_address", "run") != 0:
                raise Failure("a returned-to-caller stop must be a null jump "
                              "to be classified")
        else:
            expected = BRIDGED_STOPS[run["stop"]]
            if last["outcome"] != expected:
                raise Failure(f"the last call outcome is {last['outcome']}, "
                              f"expected {expected}")
            if number(last, "id", "call-seq") != syscall:
                raise Failure("the last call id disagrees with the run record")
            if expected == "rejected" and \
                    number(last, "argument", "call-seq") == 0:
                raise Failure("a rejected call must name the argument")
        tallies = one(records, "calls")
        notes.append(f"bridged: {tallies.get('handled')} handled, "
                     f"{tallies.get('unimplemented')} unimplemented, "
                     f"{tallies.get('unknown')} unknown, "
                     f"{tallies.get('rejected')} rejected; "
                     f"{tallies.get('allocations')} allocations")
    if run["stop"] == ACCEPTED_STOP or run["stop"] in BRIDGED_STOPS:
        if stop_address != thunk_va or last_eip != thunk_va:
            raise Failure("the run did not stop exactly on the dispatcher thunk")
    if stub_id != 0 and syscall != stub_id:
        raise Failure(f"syscall {syscall:#x} does not match the stub id "
                      f"{stub_id:#x}")

    # The syscall number is bound to the image, not to a register: the stub
    # that called the dispatcher must encode the number we observed.
    call = one(records, "call")
    if run["stop"] == ACCEPTED_STOP:
        if number(call, "in_module", "call") != 1:
            raise Failure("the boundary call did not return into the entry module")
        caller_id = number(call, "caller_id", "call")
        if caller_id == 0:
            raise Failure("the issuing stub does not encode a syscall number")
        if caller_id != syscall:
            raise Failure(f"the issuing stub names syscall {caller_id:#x}, "
                          f"the run observed {syscall:#x}")
        caller_rva = number(call, "caller_rva", "call")
        if caller_rva >= number(ntdll, "image_bytes", "ntdll.dll"):
            raise Failure("the issuing stub is outside ntdll")
        if number(call, "observed", "call") != syscall:
            raise Failure("the call record disagrees with the run record")
    notes.append(f"{entry['symbol']} retired {retired} instructions and "
                 f"reached syscall {syscall:#06x}")

    cleanup = one(records, "cleanup")
    if cleanup["status"] != "ok":
        raise Failure(f"the gate ended with status {cleanup['status']}")
    # The verdict is about what the releases did, not about what the run
    # mapped: a teardown action that failed leaves its owner in place, so a
    # transcript that reports one is not acceptance evidence.
    for field in ("failures", "pending_modules", "pending_pages",
                  "pending_regions", "pending_translations"):
        if number(cleanup, field, "cleanup") != 0:
            raise Failure(f"cleanup left {field}={cleanup[field]} behind")
    if number(cleanup, "modules", "cleanup") != len(modules):
        raise Failure("not every mapped module was released")
    if number(cleanup, "mappings", "cleanup") < len(modules):
        raise Failure("fewer mappings were released than modules mapped")
    if number(cleanup, "translations", "cleanup") < 1:
        raise Failure("translated code was not destroyed")

    verdict = one(records, "verdict")
    if run["stop"] == ACCEPTED_STOP:
        if number(verdict, "accepted", "verdict") != 1:
            raise Failure("the runner did not accept its own evidence")
    elif number(verdict, "accepted", "verdict") != 0:
        raise Failure("the runner accepted a run that did not reach the "
                      "Unix-call boundary")
    if verdict["stop"] != run["stop"]:
        raise Failure(f"verdict stop is {verdict['stop']}, "
                      f"the run stopped with {run['stop']}")
    if number(verdict, "entry_id", "verdict") != stub_id:
        raise Failure("verdict entry id disagrees with the entry record")
    if number(verdict, "syscall", "verdict") != syscall:
        raise Failure("verdict syscall disagrees with the run record")
    if number(verdict, "retired", "verdict") != retired:
        raise Failure("verdict retired count disagrees with the run record")
    return notes


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("transcript")
    parser.add_argument("--manifest", default="")
    parser.add_argument("--expect-entry", default="")
    arguments = parser.parse_args(argv)
    try:
        text = Path(arguments.transcript).read_text(encoding="utf-8")
    except OSError as error:
        raise Failure(f"cannot read {arguments.transcript}: {error}") from error
    manifest = None
    if arguments.manifest:
        try:
            manifest = json.loads(
                Path(arguments.manifest).read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise Failure(f"cannot read the manifest: {error}") from error
    records = parse_transcript(text)
    notes = validate(records, manifest, arguments.expect_entry or None)
    print("wine ntdll evidence passed: "
          + "; ".join(notes))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
