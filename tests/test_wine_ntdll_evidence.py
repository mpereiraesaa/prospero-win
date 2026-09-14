#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Adversarial tests for the Wine ntdll entry evidence contract.

The transcript is the only proof that the milestone happened, so each rule
that could be satisfied by a fake, a wrong module or a partial run has to
fail closed here.
"""

from __future__ import annotations

import copy
import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "wine_ntdll_validator", ROOT / "tools/validate_wine_ntdll_evidence.py")
assert SPEC and SPEC.loader
VALIDATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATOR)

NTDLL_BASE = 0x10404000
NTDLL_SHA = "228f0cbc6b52f8e94c760b636a3d68909eb3618183cb6cb1d2604784201195a6"
KERNELBASE_SHA = "8171a6b10c9b3acdfae29ec3ae9c30052266eda711d1fe69cdd89f78e8e3f770"
SLOT_RVA = 0xAF030
THUNK_RVA = 0xE344
ENTRY_RVA = 0xD354


def base_records() -> dict[str, list[dict[str, str]]]:
    return {
        "gate": [{"runtime": ".deps/wine-runtime/lib/i386-windows",
                  "root": "kernelbase.dll", "entry": "ntdll.dll!NtClose",
                  "modules": "2"}],
        "module": [
            {"name": "ntdll.dll", "sha256": NTDLL_SHA, "size": "3747857",
             "machine": "0x014c", "base": hex(NTDLL_BASE),
             "image_bytes": "4194304", "loaded": "1", "runtime": "1",
             "imports": "0", "tls": "0",
             "path": ".deps/wine-runtime/lib/i386-windows/ntdll.dll"},
            {"name": "kernelbase.dll", "sha256": KERNELBASE_SHA,
             "size": "4806318", "machine": "0x014c", "base": "0x10000000",
             "image_bytes": "5242880", "loaded": "1", "runtime": "1",
             "imports": "428", "tls": "0",
             "path": ".deps/wine-runtime/lib/i386-windows/kernelbase.dll"},
        ],
        "bind": [{"modules": "1", "functions": "428", "data": "0",
                  "failures": "0"}],
        "tls": [{"modules": "0"}],
        "boundary": [{"slot_rva": hex(SLOT_RVA), "slot_va": hex(NTDLL_BASE + SLOT_RVA),
                      "thunks": "1", "thunk_rva": hex(THUNK_RVA),
                      "thunk_va": hex(NTDLL_BASE + THUNK_RVA)}],
        "entry": [{"module": "ntdll.dll", "symbol": "NtClose",
                   "rva": hex(ENTRY_RVA), "eip": hex(NTDLL_BASE + ENTRY_RVA),
                   "pe_entry_rva": "0x00010c60", "stub_id": "0x0000000f",
                   "kind": "stub"}],
        "call": [{"return_eip": hex(NTDLL_BASE + ENTRY_RVA + 12),
                  "in_module": "1", "caller_rva": hex(ENTRY_RVA),
                  "caller_id": "0x0000000f", "observed": "0x0000000f"}],
        "modes": [{"chaining": "1", "residency": "1", "lazy_flags": "1"}],
        "run": [{"first_eip": hex(NTDLL_BASE + ENTRY_RVA),
                 "last_eip": hex(NTDLL_BASE + THUNK_RVA), "retired": "3",
                 "dispatches": "1", "blocks": "1", "bytes": "160",
                 "reg_loads": "3", "reg_stores": "0",
                 "reg_reconciliations": "0", "reg_spills": "0",
                 "stop_address": hex(NTDLL_BASE + THUNK_RVA),
                 "stop": VALIDATOR.ACCEPTED_STOP, "syscall": "0x0000000f",
                 "host_calls": "0"}],
        "cleanup": [{"modules": "2", "pending_modules": "0",
                     "pending_pages": "0", "pending_regions": "0",
                     "mappings": "3", "translations": "1",
                     "pending_translations": "0", "failures": "0",
                     "status": "ok"}],
        "verdict": [{"accepted": "1", "stop": VALIDATOR.ACCEPTED_STOP,
                     "entry_id": "0x0000000f", "syscall": "0x0000000f",
                     "retired": "3"}],
    }


def render(records: dict[str, list[dict[str, str]]]) -> str:
    lines = ["HELLO ps5log/1 title=PPSA99994 app=prospero-win boot=0x1 tag=test"]
    for kind in ("gate", "module", "bind", "tls", "boundary", "entry", "run",
                 "call", "modes", "cleanup", "verdict"):
        for index, record in enumerate(records.get(kind, [])):
            fields = " ".join(f"{key}={value}" for key, value in record.items())
            lines.append(f"{index}\t0\tINFO\tkind=host-wine-{kind} {fields}")
    lines.append("BYE seq=9 reason=evidence")
    return "\n".join(lines) + "\n"


def manifest() -> dict:
    return {
        "schema": "prospero-win-wine-runtime-manifest/1",
        "modules": [
            {"name": "ntdll.dll", "sha256": NTDLL_SHA, "size": 3747857},
            {"name": "kernelbase.dll", "sha256": KERNELBASE_SHA,
             "size": 4806318},
        ],
        "distribution_sha256": "0" * 64,
    }


class Case(unittest.TestCase):
    def run_validation(self, records, expect_entry: str | None = None,
                       with_manifest: bool = False) -> list[str]:
        return VALIDATOR.validate(records, manifest() if with_manifest else None,
                                  expect_entry)

    def expect_failure(self, message: str, mutate, with_manifest: bool = False,
                       expect_entry: str | None = None) -> None:
        records = base_records()
        mutate(records)
        with self.assertRaises(SystemExit) as caught:
            self.run_validation(records, expect_entry, with_manifest)
        self.assertIn(message, str(caught.exception))

    def test_accepts_a_complete_transcript(self) -> None:
        notes = self.run_validation(base_records())
        self.assertTrue(any("retired 3" in note for note in notes))

    def test_accepts_with_a_matching_manifest(self) -> None:
        self.run_validation(base_records(), with_manifest=True)

    def test_manifest_disagreement(self) -> None:
        def mutate(records):
            records["module"][0]["sha256"] = "4" * 64
        self.expect_failure("does not match the manifest", mutate,
                            with_manifest=True)

    def test_manifest_unknown_module(self) -> None:
        def mutate(records):
            records["module"][1]["name"] = "kernel32.dll"
        self.expect_failure("is not in the manifest", mutate,
                            with_manifest=True)

    def test_duplicate_module(self) -> None:
        def mutate(records):
            records["module"].append(dict(records["module"][0]))
        self.expect_failure("duplicate module record", mutate)

    def test_missing_ntdll(self) -> None:
        def mutate(records):
            records["module"] = [records["module"][1]]
        self.expect_failure("at least two module", mutate)

    def test_wrong_machine(self) -> None:
        def mutate(records):
            records["module"][0]["machine"] = "0x8664"
        self.expect_failure("not an i386 PE image", mutate)

    def test_module_never_mapped(self) -> None:
        def mutate(records):
            records["module"][0]["loaded"] = "0"
        self.expect_failure("was never mapped", mutate)

    def test_host_module_instead_of_runtime(self) -> None:
        def mutate(records):
            records["module"][0]["runtime"] = "0"
        self.expect_failure("not loaded from the runtime namespace", mutate)

    def test_empty_hash(self) -> None:
        def mutate(records):
            records["module"][0]["sha256"] = ""
        self.expect_failure("has no SHA-256", mutate)

    def test_binding_failure(self) -> None:
        def mutate(records):
            records["bind"][0]["failures"] = "1"
        self.expect_failure("import binding reported failures", mutate)

    def test_binding_did_nothing(self) -> None:
        def mutate(records):
            records["bind"][0]["functions"] = "0"
        self.expect_failure("no runtime-to-runtime import was bound", mutate)

    def test_missing_boundary(self) -> None:
        def mutate(records):
            records["boundary"][0]["thunks"] = "0"
        self.expect_failure("no __wine_syscall dispatcher thunk", mutate)

    def test_boundary_outside_ntdll(self) -> None:
        def mutate(records):
            records["boundary"][0]["thunk_va"] = hex(NTDLL_BASE + THUNK_RVA + 4)
        self.expect_failure("not inside mapped ntdll", mutate)

    def test_entry_outside_ntdll(self) -> None:
        def mutate(records):
            records["entry"][0]["eip"] = "0x00100000"
        self.expect_failure("initial guest EIP is not inside mapped ntdll",
                            mutate)

    def test_wrong_entry_symbol(self) -> None:
        self.expect_failure(
            "entry symbol is NtClose, expected LdrInitializeThunk",
            lambda r: None, expect_entry="LdrInitializeThunk")

    def test_stub_without_syscall_id(self) -> None:
        def mutate(records):
            records["entry"][0]["stub_id"] = "0x00000000"
        self.expect_failure("does not encode a syscall number", mutate)

    def test_zero_retired(self) -> None:
        def mutate(records):
            records["run"][0]["retired"] = "0"
        self.expect_failure("no guest instruction was retired", mutate)

    def test_host_call(self) -> None:
        def mutate(records):
            records["run"][0]["host_calls"] = "1"
        self.expect_failure("a host Wine function was called", mutate)

    def test_wrong_stop(self) -> None:
        def mutate(records):
            records["run"][0]["stop"] = "decode-failure"
        self.expect_failure("the gate stopped with decode-failure", mutate)

    def test_instruction_stop_without_a_bridge(self) -> None:
        def mutate(records):
            records["run"][0]["stop"] = "unsupported-instruction"
        self.expect_failure("must report exactly one calls record", mutate)

    def test_stop_not_on_the_thunk(self) -> None:
        def mutate(records):
            records["run"][0]["stop_address"] = hex(NTDLL_BASE + ENTRY_RVA)
        self.expect_failure("did not stop exactly on the dispatcher thunk",
                            mutate)

    def test_syscall_mismatch(self) -> None:
        def mutate(records):
            records["run"][0]["syscall"] = "0x00000010"
        self.expect_failure("does not match the stub id", mutate)

    def test_wrong_start(self) -> None:
        def mutate(records):
            records["run"][0]["first_eip"] = hex(NTDLL_BASE + THUNK_RVA)
        self.expect_failure("did not start at the entry point", mutate)

    def test_leaked_mappings(self) -> None:
        def mutate(records):
            records["cleanup"][0]["modules"] = "1"
        self.expect_failure("not every mapped module was released", mutate)

    def test_leaked_translations(self) -> None:
        def mutate(records):
            records["cleanup"][0]["translations"] = "0"
        self.expect_failure("translated code was not destroyed", mutate)

    def test_cleanup_status(self) -> None:
        def mutate(records):
            records["cleanup"][0]["status"] = "vm"
        self.expect_failure("ended with status vm", mutate)

    def test_cleanup_release_failure(self) -> None:
        """A teardown that failed leaves its owner behind: not acceptance."""
        def mutate(records):
            records["cleanup"][0]["failures"] = "1"
        self.expect_failure("cleanup left failures=1 behind", mutate)

    def test_cleanup_pending_module(self) -> None:
        def mutate(records):
            records["cleanup"][0]["pending_modules"] = "1"
        self.expect_failure("cleanup left pending_modules=1 behind", mutate)

    def test_cleanup_pending_page(self) -> None:
        def mutate(records):
            records["cleanup"][0]["pending_pages"] = "3"
        self.expect_failure("cleanup left pending_pages=3 behind", mutate)

    def test_cleanup_pending_call_region(self) -> None:
        def mutate(records):
            records["cleanup"][0]["pending_regions"] = "1"
        self.expect_failure("cleanup left pending_regions=1 behind", mutate)

    def test_cleanup_pending_translation(self) -> None:
        def mutate(records):
            records["cleanup"][0]["pending_translations"] = "1"
        self.expect_failure("cleanup left pending_translations=1 behind",
                            mutate)

    def test_verdict_not_accepted(self) -> None:
        def mutate(records):
            records["verdict"][0]["accepted"] = "0"
        self.expect_failure("did not accept its own evidence", mutate)

    def test_verdict_disagrees(self) -> None:
        def mutate(records):
            records["verdict"][0]["retired"] = "4"
        self.expect_failure("verdict retired count disagrees", mutate)

    def test_missing_record(self) -> None:
        def mutate(records):
            del records["run"]
        self.expect_failure("expected exactly one run record", mutate)

    def test_missing_modes_record(self) -> None:
        def mutate(records):
            del records["modes"]
        self.expect_failure("expected exactly one modes record", mutate)

    def test_initialization_entry_without_a_stub_id(self) -> None:
        def mutate(records):
            records["entry"][0].update(stub_id="0x00000000",
                                       kind="initialization")
            records["run"][0]["syscall"] = "0x00000018"
            records["verdict"][0].update(entry_id="0x00000000",
                                         syscall="0x00000018")
            records["call"][0].update(caller_id="0x00000018",
                                      observed="0x00000018")
        records = base_records()
        mutate(records)
        # The initialization shape is accepted, and the syscall number is
        # still bound to the stub that issued it.
        notes = self.run_validation(records)
        self.assertTrue(notes)

    def test_initialization_entry_claiming_a_stub_id(self) -> None:
        def mutate(records):
            records["entry"][0].update(stub_id="0x0000000f",
                                       kind="initialization")
        self.expect_failure("entry kind is 'initialization', expected stub",
                            mutate)

    def test_caller_stub_disagrees(self) -> None:
        def mutate(records):
            records["call"][0]["caller_id"] = "0x00000011"
        self.expect_failure("the issuing stub names syscall 0x11", mutate)

    def test_caller_outside_the_module(self) -> None:
        def mutate(records):
            records["call"][0]["in_module"] = "0"
        self.expect_failure("did not return into the entry module", mutate)

    def test_missing_call_record(self) -> None:
        def mutate(records):
            del records["call"]
        self.expect_failure("expected exactly one call record", mutate)

    def test_parse_rejects_malformed_field(self) -> None:
        text = render(base_records()).replace("stop=wine-unix-call-boundary",
                                              "stop", 1)
        with self.assertRaises(SystemExit) as caught:
            VALIDATOR.parse_transcript(text)
        self.assertIn("malformed field", str(caught.exception))

    def test_round_trip_through_the_parser(self) -> None:
        records = VALIDATOR.parse_transcript(render(base_records()))
        self.assertEqual(len(records["module"]), 2)
        self.run_validation(copy.deepcopy(records))

    # --- bridged runs -------------------------------------------------

    def bridged_records(self) -> dict:
        """A run that serviced one call and stopped on an instruction."""
        records = base_records()
        # The bridged run enters ntdll's initialization, so the entry has no
        # stub id of its own; the serviced calls carry the provenance.
        records["entry"][0].update(symbol="LdrInitializeThunk",
                                   rva="0x00013530",
                                   eip=hex(NTDLL_BASE + 0x13530),
                                   stub_id="0x00000000",
                                   kind="initialization")
        records["run"][0].update(stop="unsupported-instruction",
                                 stop_address="0x10420dd5",
                                 first_eip=hex(NTDLL_BASE + 0x13530),
                                 last_eip="0x10420dd5", retired="315",
                                 syscall="0x00000018")
        records["verdict"][0].update(accepted="0", stop="unsupported-instruction",
                                     entry_id="0x00000000", syscall="0x00000018",
                                     retired="315")
        records["call"][0].update(return_eip="0x00000000", in_module="0",
                                  caller_rva="0x00000000", caller_id="0x00000000",
                                  observed="0x00000018")
        records["calls"] = [{"serviced": "1", "handled": "1",
                             "unimplemented": "0", "unknown": "0",
                             "rejected": "0", "allocations": "1",
                             "allocated_bytes": "65536", "regions": "1"}]
        records["call-seq"] = [{
            "index": "0", "id": "0x00000018", "name": "NtAllocateVirtualMemory",
            "args": "24", "stub_return": hex(NTDLL_BASE + 0xD3F0),
            "return": hex(NTDLL_BASE + 0x4AED1), "status": "0x00000000",
            "outcome": "handled", "argument": "6"}]
        return records

    def test_accepts_a_bridged_run_that_stopped_on_an_instruction(self) -> None:
        notes = self.run_validation(self.bridged_records())
        self.assertTrue(any("bridged:" in note for note in notes))

    def test_bridged_first_call_must_be_the_allocator(self) -> None:
        records = self.bridged_records()
        records["call-seq"][0]["id"] = "0x00000019"
        with self.assertRaises(SystemExit) as caught:
            self.run_validation(records)
        self.assertIn("not NtAllocateVirtualMemory", str(caught.exception))

    def test_bridged_call_may_not_fail(self) -> None:
        records = self.bridged_records()
        records["call-seq"][0]["status"] = "0xc0000008"
        with self.assertRaises(SystemExit) as caught:
            self.run_validation(records)
        self.assertIn("did not succeed", str(caught.exception))

    def test_bridged_stub_must_be_inside_ntdll(self) -> None:
        records = self.bridged_records()
        records["call-seq"][0]["stub_return"] = "0x00100000"
        with self.assertRaises(SystemExit) as caught:
            self.run_validation(records)
        self.assertIn("did not return into ntdll", str(caught.exception))

    def test_bridged_stop_must_be_inside_ntdll(self) -> None:
        records = self.bridged_records()
        records["run"][0]["stop_address"] = "0x00100000"
        records["run"][0]["last_eip"] = "0x00100000"
        with self.assertRaises(SystemExit) as caught:
            self.run_validation(records)
        self.assertIn("not inside ntdll", str(caught.exception))

    def test_bridged_sequence_indices_must_be_contiguous(self) -> None:
        records = self.bridged_records()
        records["call-seq"].append(dict(records["call-seq"][0], index="2"))
        with self.assertRaises(SystemExit) as caught:
            self.run_validation(records)
        self.assertIn("not contiguous", str(caught.exception))

    def test_bridged_outcome_must_explain_the_stop(self) -> None:
        records = self.bridged_records()
        records["run"][0]["stop"] = "unix-call-unimplemented"
        records["run"][0]["syscall"] = "0x00000019"
        records["verdict"][0]["stop"] = "unix-call-unimplemented"
        records["verdict"][0]["syscall"] = "0x00000019"
        records["call-seq"].append({
            "index": "1", "id": "0x00000019",
            "name": "NtQueryInformationProcess", "args": "20",
            "stub_return": hex(NTDLL_BASE + 0xD420),
            "return": hex(NTDLL_BASE + 0x4EF03), "status": "0xc0000002",
            "outcome": "handled", "argument": "0"})
        with self.assertRaises(SystemExit) as caught:
            self.run_validation(records)
        self.assertIn("last call outcome is handled", str(caught.exception))

    def test_accepts_a_run_the_guest_ended_itself(self) -> None:
        """A run that ends because the guest terminated its own process."""
        records = self.bridged_records()
        records["run"][0].update(stop="process-terminated",
                                 stop_address=hex(NTDLL_BASE + THUNK_RVA),
                                 last_eip=hex(NTDLL_BASE + THUNK_RVA),
                                 syscall="0x0000002c")
        records["verdict"][0].update(stop="process-terminated",
                                     syscall="0x0000002c")
        records["call"][0]["observed"] = "0x0000002c"
        records["call-seq"].append({
            "index": "1", "id": "0x0000002c", "name": "NtTerminateProcess",
            "args": "8", "stub_return": hex(NTDLL_BASE + 0xD430),
            "return": hex(NTDLL_BASE + 0x4EF03), "status": "0x00000000",
            "outcome": "handled", "argument": "2"})
        notes = self.run_validation(records)
        self.assertTrue(any("bridged:" in note for note in notes))

    def test_terminating_run_must_have_serviced_the_termination(self) -> None:
        records = self.bridged_records()
        records["run"][0].update(stop="process-terminated",
                                 stop_address=hex(NTDLL_BASE + THUNK_RVA),
                                 last_eip=hex(NTDLL_BASE + THUNK_RVA),
                                 syscall="0x0000002c")
        records["verdict"][0].update(stop="process-terminated",
                                     syscall="0x0000002c")
        records["call"][0]["observed"] = "0x0000002c"
        records["call-seq"].append({
            "index": "1", "id": "0x0000002c", "name": "NtTerminateProcess",
            "args": "8", "stub_return": hex(NTDLL_BASE + 0xD430),
            "return": hex(NTDLL_BASE + 0x4EF03), "status": "0xc0000002",
            "outcome": "unimplemented", "argument": "0"})
        with self.assertRaises(SystemExit) as caught:
            self.run_validation(records)
        self.assertIn("last call outcome is unimplemented",
                      str(caught.exception))

    # --- classified memory stops --------------------------------------

    def memory_stop_records(self) -> dict:
        """A run that serviced calls and then dereferenced a null guest field."""
        records = self.bridged_records()
        records["run"][0].update(stop="memory-bounds",
                                 stop_address=hex(NTDLL_BASE + 0x24EE6),
                                 last_eip=hex(NTDLL_BASE + 0x24EE6))
        records["verdict"][0].update(stop="memory-bounds")
        records["fault"] = [{"address": "0x000007ca", "width": "2",
                             "write": "0"}]
        return records

    def test_accepts_a_classified_memory_stop(self) -> None:
        notes = self.run_validation(self.memory_stop_records())
        self.assertTrue(any("bridged:" in note for note in notes))

    def test_memory_stop_needs_a_fault_record(self) -> None:
        records = self.memory_stop_records()
        del records["fault"]
        with self.assertRaises(SystemExit) as caught:
            self.run_validation(records)
        self.assertIn("expected exactly one fault record", str(caught.exception))

    def test_memory_stop_inside_a_module_is_not_classified(self) -> None:
        records = self.memory_stop_records()
        records["fault"][0]["address"] = hex(NTDLL_BASE + 0x1000)
        with self.assertRaises(SystemExit) as caught:
            self.run_validation(records)
        self.assertIn("inside a mapped module", str(caught.exception))

    def test_memory_stop_with_a_refused_call(self) -> None:
        records = self.memory_stop_records()
        records["call-seq"].append({
            "index": "1", "id": "0x00000019",
            "name": "NtQueryInformationProcess", "args": "20",
            "stub_return": hex(NTDLL_BASE + 0xD420),
            "return": hex(NTDLL_BASE + 0x4EF03), "status": "0xc0000002",
            "outcome": "unimplemented", "argument": "0"})
        with self.assertRaises(SystemExit) as caught:
            self.run_validation(records)
        self.assertIn("may not report a refused call", str(caught.exception))

    def test_memory_stop_must_name_an_access_width(self) -> None:
        records = self.memory_stop_records()
        records["fault"][0]["width"] = "3"
        with self.assertRaises(SystemExit) as caught:
            self.run_validation(records)
        self.assertIn("does not describe an access", str(caught.exception))

    def test_accepts_a_translation_arena_stop(self) -> None:
        records = self.memory_stop_records()
        del records["fault"]
        records["run"][0]["stop"] = "cache-limit"
        records["verdict"][0]["stop"] = "cache-limit"
        notes = self.run_validation(records)
        self.assertTrue(any("bridged:" in note for note in notes))

    def test_arena_stop_with_a_refused_call(self) -> None:
        records = self.memory_stop_records()
        del records["fault"]
        records["run"][0]["stop"] = "cache-limit"
        records["verdict"][0]["stop"] = "cache-limit"
        records["call-seq"].append({
            "index": "1", "id": "0x00000019",
            "name": "NtQueryInformationProcess", "args": "20",
            "stub_return": hex(NTDLL_BASE + 0xD420),
            "return": hex(NTDLL_BASE + 0x4EF03), "status": "0xc0000002",
            "outcome": "rejected", "argument": "2"})
        with self.assertRaises(SystemExit) as caught:
            self.run_validation(records)
        self.assertIn("may not report a refused call", str(caught.exception))

    def test_accepts_a_classified_null_jump(self) -> None:
        records = self.memory_stop_records()
        del records["fault"]
        records["run"][0].update(stop="returned-to-caller", stop_address="0x00000000",
                                 last_eip="0x00000000")
        records["verdict"][0]["stop"] = "returned-to-caller"
        notes = self.run_validation(records)
        self.assertTrue(any("bridged:" in note for note in notes))

    def test_null_jump_must_be_exactly_zero(self) -> None:
        records = self.memory_stop_records()
        del records["fault"]
        records["run"][0].update(stop="returned-to-caller",
                                 stop_address="0x00001000",
                                 last_eip="0x00001000")
        records["verdict"][0]["stop"] = "returned-to-caller"
        with self.assertRaises(SystemExit) as caught:
            self.run_validation(records)
        self.assertIn("must be a null jump", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
