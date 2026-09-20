#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Every sanitized startup x87 form has a translated synthetic representative."""
import json
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[1]
forms = {
    "FABS:op1/reg/g4/r1": "d9e1",
    "FADD:op0/mem/g0": "d80500000000",
    "FADD:op4/mem/g0": "dc0500000000",
    "FADDP:op6/reg/g0/r1": "dec1",
    "FCOM:op0/mem/g2": "d81500000000",
    "FCOM:op0/reg/g2/r1": "d8d1",
    "FCOM:op4/mem/g2": "dc1500000000",
    "FCOMP:op0/mem/g3": "d81d00000000",
    "FCOMP:op4/mem/g3": "dc1d00000000",
    "FDIV:op0/mem/g6": "d83500000000",
    "FDIV:op0/reg/g6/r1": "d8f1",
    "FDIVP:op6/reg/g7/r1": "def9",
    "FDIVR:op4/mem/g7": "dc3d00000000",
    "FILD:op3/mem/g0": "db0500000000",
    "FLD1:op1/reg/g5/r0": "d9e8",
    "FLD:op1/mem/g0": "d90500000000",
    "FLD:op1/reg/g0/r0": "d9c0",
    "FLD:op1/reg/g0/r1": "d9c1",
    "FLD:op1/reg/g0/r2": "d9c2",
    "FLD:op5/mem/g0": "dd0500000000",
    "FLDZ:op1/reg/g5/r6": "d9ee",
    "FMUL:op0/mem/g1": "d80d00000000",
    "FMUL:op0/reg/g1/r1": "d8c9",
    "FMUL:op0/reg/g1/r3": "d8cb",
    "FMUL:op4/mem/g1": "dc0d00000000",
    "FNSTSW:op7/reg/g4/r0": "dfe0",
    "FSQRT:op1/reg/g7/r2": "d9fa",
    "FST:op1/mem/g2": "d91500000000",
    "FSTP:op1/mem/g3": "d91d00000000",
    "FSTP:op5/mem/g3": "dd1d00000000",
    "FSTP:op5/reg/g3/r0": "ddd8",
    "FSTP:op5/reg/g3/r2": "ddda",
    "FSUB:op0/mem/g4": "d82500000000",
    "FSUB:op0/reg/g4/r1": "d8e1",
    "FSUBR:op0/mem/g5": "d82d00000000",
    "FUCOMPP:op2/reg/g5/r1": "dae9",
}
coverage = json.loads(
    (root / "tests/fixtures/pinball_x86_coverage.json").read_text()
)
startup = coverage["roots"]["startup"]
observed = {name for name, _ in startup["x87_forms"]}
supported = {name for name, _ in startup["x87_supported_forms"]}
assert len(forms) == 36 and observed == supported == set(forms)
assert startup["x87_unsupported_forms"] == []
result = subprocess.run(
    [str(root / "build/host/classify_x86")],
    input="".join(value + "\n" for value in forms.values()),
    text=True, capture_output=True, check=True)
statuses = [int(line) for line in result.stdout.splitlines()]
assert len(statuses) == len(forms) and all(status == 0 for status in statuses), statuses
print("startup x87 contract passed: 36/36 sanitized forms translated")
