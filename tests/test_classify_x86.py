#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""The coverage classifier must use the translator's exact acceptance."""
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tools"))
from survey_x86_coverage import parse_root, reachable, x87_form

assert reachable({"a": {"b", "c"}, "b": {"c"}}, "a") == {"a", "b", "c"}
parsed = parse_root("01020f95=entry")
assert (parsed.address, parsed.label) == ("01020f95", "entry")
result = subprocess.run(
    [str(root / "build/host/classify_x86")],
    input="8bff\n55\nd9e8\nd9fc\nzz\n\n"
          "0fca\n660fca\n"
          "660ffdc1\n660f74c1\n660fd7c1\n660f6e03\n660f7e03\n660ff9c1\n"
          "660ff7c1\n660fe7c1\n"
          "0f28c3\n0f29c3\n660f6fc3\n660f7fc3\n"
          "0f2810\n0f2910\n660f6f10\n660f7f10\n"
          "8711\nf08711\n87d0\n8610\n668711\n",
    text=True, capture_output=True, check=True)
statuses = [int(line) for line in result.stdout.splitlines()]
assert statuses[:3] == [0, 0, 0], statuses
assert statuses[3] != 0, statuses
assert statuses[4:6] == [-1, -1], statuses
# bswap, the packed-integer arithmetic and comparison forms and the two movd
# directions are translated; the undefined 16-bit bswap and the SSE opcodes
# that store through a memory operand are refused rather than misread.
assert statuses[6] == 0, statuses
assert statuses[7] != 0, statuses
assert statuses[8:14] == [0] * 6, statuses
assert statuses[14] != 0 and statuses[15] != 0, statuses
# The packed moves that require 16-byte alignment are accepted between
# registers and with a memory operand: the block checks the address first and
# a misaligned one is the guest's own access violation.
assert statuses[16:20] == [0] * 4, statuses
assert statuses[20:24] == [0] * 4, statuses
# XCHG: the memory form Wine's heap code uses to take an entry off a free list
# is translated, with or without the redundant LOCK prefix; the register form
# has no memory operand to exchange and the byte and 16-bit forms are not
# implemented, so all three are refused rather than half-translated.
assert statuses[24:26] == [0, 0], statuses
assert statuses[26:29] == [-5, -5, -5], statuses
print("x86 instruction classifier passed: exact supported and rejected forms")

assert x87_form(bytes.fromhex("d9e8"), "FLD1") == "FLD1:op1/reg/g5/r0"
assert x87_form(bytes.fromhex("9bdbe3"), "FINIT") == "FINIT:op3/reg/g4/r3"
assert x87_form(bytes.fromhex("d945fc"), "FLD") == "FLD:op1/mem/g0"
assert x87_form(bytes.fromhex("f3d945fc"), "FLD") == "FLD:op1/mem/g0"
assert x87_form(bytes.fromhex("90"), "NOP") is None
print("x87 form classifier passed")
