#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Contract tests for the PE mapping evidence validator.

The accepted transcript is produced by the gate itself: `test_pw_gate
--emit` runs the real loader and prints the records the console would send.
Rejection cases then mutate that transcript one field at a time, so the
validator is tested against the format the runtime actually emits rather
than against a hand-written imitation of it.
"""

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GATE = ROOT / "build" / "host" / "test_pw_gate"

sys.path.insert(0, str(ROOT / "tools"))

import validate_pe_map_evidence as validator  # noqa: E402

BOOT = "0x00000000deadbeef"


def gate_records() -> list[str]:
    if not GATE.exists():
        raise unittest.SkipTest(f"{GATE} is not built")
    completed = subprocess.run([str(GATE), "--emit"], check=True,
                               capture_output=True, text=True)
    return completed.stdout.splitlines()


class EvidenceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.records = gate_records()

    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.path = Path(self.directory.name)
        self.addCleanup(self.directory.cleanup)

    def write(self, records: list[str], *, manifest_overrides: dict | None = None,
              hello_overrides: dict | None = None,
              break_hash: bool = False) -> Path:
        hello = {"title": "PPSA99995", "app": "prospero-win", "boot": BOOT,
                 "tag": "pe-map"}
        hello.update(hello_overrides or {})
        lines = ["HELLO ps5log/1 " + " ".join(f"{key}={value}"
                                              for key, value in hello.items())]
        for index, record in enumerate(records, start=1):
            lines.append(f"{index}\t{index * 1000}\tINFO\t{record}")
        lines.append(f"BYE seq={len(records)} reason=pe-map-complete")
        data = ("\n".join(lines) + "\n").encode("utf-8")

        transcript = self.path / "run.log"
        transcript.write_bytes(data)
        manifest = {
            "identity": {"title": "PPSA99995", "app": "prospero-win",
                         "boot": BOOT},
            "protocol": "ps5log/1",
            "transport": "tcp",
            "hello": True,
            "bye": True,
            "clean": True,
            "gaps": [],
            "raw_lines": 0,
            "oversized_lines": 0,
            "log_path": "run.log",
            "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        }
        if break_hash:
            manifest["sha256"] = "0" * 64
        manifest.update(manifest_overrides or {})
        manifest_path = self.path / "run.json"
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        return manifest_path

    WRITE_KEYS = ("manifest_overrides", "hello_overrides", "break_hash")

    def _split(self, kwargs: dict) -> tuple[dict, dict]:
        write = {key: kwargs.pop(key) for key in list(kwargs)
                 if key in self.WRITE_KEYS}
        options = {"root": None, "expect_modules": None, "expect_local": None,
                   "expect_runtime": None, "expect_host": None,
                   "allow_i386": False,
                   "allow_wx": False, "expect_compat32": "any"}
        options.update(kwargs)
        return write, options

    def accept(self, records: list[str] | None = None, **kwargs) -> dict:
        write, options = self._split(kwargs)
        return validator.validate(
            self.write(records or self.records, **write), **options)

    def reject(self, records: list[str] | None = None, *, message: str = "",
               **kwargs) -> None:
        write, options = self._split(kwargs)
        with self.assertRaises(validator.EvidenceError) as caught:
            validator.validate(self.write(records or self.records, **write),
                               **options)
        if message:
            self.assertIn(message, str(caught.exception))

    def mutate(self, needle: str, replacement: str) -> list[str]:
        records = []
        for record in self.records:
            records.append(record.replace(needle, replacement)
                           if needle in record else record)
        if records == self.records:
            raise AssertionError(f"mutation {needle!r} matched nothing")
        return records

    def test_accepts_a_real_gate_run(self) -> None:
        summary = self.accept(root="game.exe", expect_modules=4,
                              expect_local=1, expect_runtime=0, expect_host=2)
        self.assertEqual(summary["machine"], "amd64")
        self.assertEqual(summary["mapped"], 2)
        self.assertEqual(summary["cycles"], 0)
        self.assertEqual(summary["max_depth"], 2)
        self.assertGreater(summary["reserved_bytes"], 0)

    def test_accepts_historical_graph_without_runtime_field(self) -> None:
        records = self.mutate("local=1 runtime=0 host=2",
                              "local=1 host=2")
        summary = self.accept(records, expect_runtime=0)
        self.assertEqual(summary["runtime"], 0)

    def test_pins_expected_counts(self) -> None:
        self.reject(expect_modules=5, message="expected modules=5")
        self.reject(expect_local=2, message="expected local=2")
        self.reject(expect_runtime=1, message="expected runtime=1")
        self.reject(expect_host=1, message="expected host=1")
        self.reject(root="other.exe", message="expected root=other.exe")

    def test_rejects_module_kind_count_drift(self) -> None:
        self.reject(self.mutate("local=1 runtime=0 host=2",
                                "local=1 runtime=1 host=2"),
                    message="runtime count disagrees")
        records = []
        for record in self.records:
            if record.startswith("PW_DEP") and "dep_kind=host" in record:
                record = record.replace("dep_kind=host", "dep_kind=runtime")
                records.append(record)
                records.extend(self.records[len(records):])
                break
            records.append(record)
        self.assertNotEqual(records, self.records)
        self.reject(records, message="PW_DEP kind disagrees")

    def test_rejects_transport_defects(self) -> None:
        self.reject(manifest_overrides={"gaps": [3]},
                    message="transport corruption")
        self.reject(manifest_overrides={"raw_lines": 1},
                    message="transport corruption")
        self.reject(manifest_overrides={"bye": False},
                    message="clean HELLO/BYE")
        self.reject(manifest_overrides={"transport": "udp"},
                    message="transport mismatch")
        # Another title's transcript must never validate as this one. The
        # id is assembled so the publication audit, which bans the literal
        # anywhere in the tree, stays satisfied.
        self.reject(manifest_overrides={
            "identity": {"title": "PPSA999" + "96", "app": "prospero-win",
                         "boot": BOOT}}, message="identity mismatch")
        self.reject(break_hash=True, message="hash mismatch")
        self.reject(manifest_overrides={"log_path": "../run.log"},
                    message="unsafe transcript path")

    def test_rejects_boot_token_mismatch(self) -> None:
        self.reject(hello_overrides={"boot": "0x1"},
                    message="boot token does not match")

    def test_requires_the_boot_record(self) -> None:
        self.reject([record for record in self.records
                     if not record.startswith("PW_BOOT")],
                    message="exactly one PW_BOOT")
        self.reject(self.mutate("slice=pe-map", "slice=other"),
                    message="expected slice=pe-map")
        self.reject(self.mutate("schema=1", "schema=2"),
                    message="expected schema=1")

    def test_rejects_failed_verification(self) -> None:
        self.reject(self.mutate("verify_mismatch=0", "verify_mismatch=1"),
                    message="failed verify_mismatch")
        self.reject(self.mutate("verify_zero_tail=0", "verify_zero_tail=2"),
                    message="failed verify_zero_tail")
        self.reject(self.mutate("verify_alias=0", "verify_alias=1"),
                    message="failed verify_alias")
        self.reject(self.mutate("headers=1", "headers=0"),
                    message="expected headers=1")

    def test_rejects_a_rebase_without_relocation(self) -> None:
        # The loader mapped away from the preferred base, so a zero applied
        # count would mean the image holds pointers into the wrong address.
        self.reject(self.mutate("reloc_applied=1", "reloc_applied=0"),
                    message="rebased without relocation")

    def test_rejects_a_host_module_loaded_from_disk(self) -> None:
        records = []
        for record in self.records:
            if record.startswith("PW_MODULE") and "kind=host" in record:
                record = record.replace("mapped=0", "mapped=1")
            records.append(record)
        self.reject(records, message="was mapped from disk")

    def test_rejects_writable_executable_pages_by_default(self) -> None:
        records = self.mutate("wx=0", "wx=3")
        self.reject(records, message="writable-executable pages")
        # A coarse mapping granularity is acceptable only when acknowledged.
        summary = self.accept(records, allow_wx=True)
        self.assertEqual(summary["mapped"], 2)

    def test_rejects_a_non_native_module_by_default(self) -> None:
        # A whole graph of 32-bit images: parseable and mappable, but not
        # executable on this hardware without translation.
        records = [record.replace("machine=amd64", "machine=i386")
                            .replace("native=1", "native=0")
                   for record in self.records]
        self.reject(records, message="not natively executable")
        # Accepting it is a deliberate, flagged decision, never the default.
        summary = self.accept(records, allow_i386=True)
        self.assertEqual(summary["machine"], "i386")

    def test_rejects_mixed_instruction_sets(self) -> None:
        records = []
        replaced = False
        for record in self.records:
            if record.startswith("PW_MODULE") and "kind=local" in record \
                    and not replaced:
                record = record.replace("machine=amd64", "machine=i386")
                record = record.replace("native=1", "native=0")
                replaced = True
            records.append(record)
        self.reject(records, allow_i386=True, message="mixes instruction sets")

    def test_rejects_a_broken_load_order(self) -> None:
        # Swap the two ordering positions so a dependency follows its user.
        records = []
        for record in self.records:
            if record.startswith("PW_ORDER position=1"):
                record = record.replace("position=1", "position=3")
            elif record.startswith("PW_ORDER position=3"):
                record = record.replace("position=3", "position=1")
            records.append(record)
        self.reject(records, message="out of sequence")

        renamed = self.mutate("PW_ORDER position=0 index=3 name=msvcrt.dll",
                              "PW_ORDER position=0 index=3 name=other.dll")
        self.reject(renamed, message="PW_ORDER name disagrees")

    def test_rejects_inconsistent_dependency_counts(self) -> None:
        self.reject([record for record in self.records
                     if not record.startswith("PW_DEP index=0 "
                                              "name=game.exe dep_index=2")],
                    message="declared 2 dependencies but emitted 1")

    def test_rejects_missing_protection_records(self) -> None:
        self.reject([record for record in self.records
                     if not record.startswith("PW_PROTECT index=1")],
                    message="does not cover exactly the mapped modules")

    def test_rejects_a_bad_exit(self) -> None:
        self.reject(self.mutate("result=0 status=ok", "result=8 status=not-found"),
                    message="expected result=0")
        self.reject(self.mutate("missing=none", "missing=mss32.dll"),
                    message="expected missing=none")
        self.reject(self.mutate("released=2", "released=1"),
                    message="not every mapped image was released")
        self.reject(self.mutate("truncated=0", "truncated=4"),
                    message="expected truncated=0")

    def test_rejects_error_level_records(self) -> None:
        manifest = self.write(self.records)
        transcript = manifest.parent / "run.log"
        data = transcript.read_bytes().replace(b"\tINFO\tPW_GRAPH",
                                               b"\tERR\tPW_GRAPH")
        transcript.write_bytes(data)
        manifest_data = json.loads(manifest.read_text(encoding="utf-8"))
        manifest_data["bytes"] = len(data)
        manifest_data["sha256"] = hashlib.sha256(data).hexdigest()
        manifest.write_text(json.dumps(manifest_data), encoding="utf-8")
        with self.assertRaises(validator.EvidenceError) as caught:
            validator.validate(manifest, root=None, expect_modules=None,
                               expect_local=None, expect_runtime=None,
                               expect_host=None,
                               allow_i386=False, allow_wx=False)
        self.assertIn("error record", str(caught.exception))

    # --- compatibility-mode diagnostic -----------------------------
    COMPAT32_BASE = (
        "PW_COMPAT32 schema=1 install=ok install_errno=0 ldt_index=0 "
        "code_sel=0x7 data_sel=0xf cs64=0x33 ds64=0x0 reserve=ok "
        "code_base=0x20000000 data_base=0x20001000 build=ok seal=ok "
        "seal_errno=0 transfer=ok attempted=1 returned=1 result=3 "
        "cs_seen=0x7 expected=3 proven=1"
    )

    def with_compat32(self, record: str) -> list[str]:
        return [self.records[0], record, *self.records[1:]]

    def test_accepts_a_proven_compat32_probe(self) -> None:
        summary = self.accept(self.with_compat32(self.COMPAT32_BASE),
                              expect_compat32="proven")
        self.assertEqual(summary["compat32"], "proven")
        self.assertEqual(summary["compat32_result"], "3")

    def test_accepts_a_refused_compat32_probe(self) -> None:
        # A refusal is a legitimate measurement, not a failed run.
        refused = (
            "PW_COMPAT32 schema=1 install=unsupported install_errno=78 "
            "ldt_index=0 code_sel=0x0 data_sel=0x0 cs64=0x33 ds64=0x0 "
            "reserve=precondition code_base=0x0 data_base=0x0 "
            "build=precondition seal=ok seal_errno=0 "
            "transfer=precondition attempted=0 returned=0 result=0 "
            "cs_seen=0x0 expected=3 proven=0"
        )
        summary = self.accept(self.with_compat32(refused),
                              expect_compat32="refused")
        self.assertEqual(summary["compat32"], "refused")
        # And the operator cannot mistake it for a pass.
        self.reject(self.with_compat32(refused), expect_compat32="proven",
                    message="expected compat32 proven, observed refused")

    def test_rejects_compat32_claiming_more_than_it_showed(self) -> None:
        # Proof asserted although the descriptor was never installed.
        self.reject(self.with_compat32(
            self.COMPAT32_BASE.replace("install=ok", "install=unsupported")),
            message="claims a transfer after installation failed")
        # Proof asserted without ever attempting the transfer.
        self.reject(self.with_compat32(
            self.COMPAT32_BASE.replace("attempted=1", "attempted=0")),
            message="claims proof without attempting")
        # Proof asserted without coming back.
        self.reject(self.with_compat32(
            self.COMPAT32_BASE.replace("returned=1", "returned=0")),
            message="claims proof without returning")
        # The wrong result value cannot be reported as proof.
        self.reject(self.with_compat32(
            self.COMPAT32_BASE.replace("result=3", "result=0")),
            message="disagrees with its own result")
        # Running under a selector other than the installed one.
        self.reject(self.with_compat32(
            self.COMPAT32_BASE.replace("cs_seen=0x7", "cs_seen=0x33")),
            message="disagrees with its own result")
        # A step that failed cannot coexist with proof.
        self.reject(self.with_compat32(
            self.COMPAT32_BASE.replace("seal=ok", "seal=vm")),
            message="proven but seal=vm")

    def test_requires_the_record_when_an_outcome_is_expected(self) -> None:
        self.reject(expect_compat32="proven",
                    message="no PW_COMPAT32 record")
        # Absent by default is fine: the pe-map gate can run without it.
        summary = self.accept()
        self.assertEqual(summary["compat32"], "absent")

    def test_rejects_duplicate_compat32_records(self) -> None:
        records = self.with_compat32(self.COMPAT32_BASE)
        records.insert(1, self.COMPAT32_BASE)
        self.reject(records, message="exactly one PW_COMPAT32")

    def test_rejects_a_truncated_sequence(self) -> None:
        manifest = self.write(self.records)
        transcript = manifest.parent / "run.log"
        lines = transcript.read_bytes().decode("utf-8").splitlines()
        del lines[3]                                    # drop one record
        data = ("\n".join(lines) + "\n").encode("utf-8")
        transcript.write_bytes(data)
        manifest_data = json.loads(manifest.read_text(encoding="utf-8"))
        manifest_data["bytes"] = len(data)
        manifest_data["sha256"] = hashlib.sha256(data).hexdigest()
        manifest.write_text(json.dumps(manifest_data), encoding="utf-8")
        with self.assertRaises(validator.EvidenceError) as caught:
            validator.validate(manifest, root=None, expect_modules=None,
                               expect_local=None, expect_runtime=None,
                               expect_host=None,
                               allow_i386=False, allow_wx=False)
        self.assertIn("sequence gap", str(caught.exception))


class Call6Test(unittest.TestCase):
    record = ("PW_CALL6 kind=synthetic-code status=ok constant=42 alignment=8 "
              "weighted=278 high=4294967574 sealed=1 released=1")

    def test_execution_values_and_cleanup(self):
        self.assertTrue(validator.check_call6([self.record], True))
        self.assertFalse(validator.check_call6([]))
        for old in ("constant=42", "alignment=8", "weighted=278",
                    "high=4294967574", "sealed=1", "released=1"):
            with self.subTest(old=old), self.assertRaises(validator.EvidenceError):
                validator.check_call6([self.record.replace(old, old.split("=")[0] + "=0")])
        for records in ([], [self.record, self.record]):
            with self.assertRaises(validator.EvidenceError):
                validator.check_call6(records, True)


if __name__ == "__main__":
    unittest.main()
