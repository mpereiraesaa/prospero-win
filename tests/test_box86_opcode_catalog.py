#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Unit tests for the deterministic Box86 dispatch inventory normalizer."""

import importlib.util
from pathlib import Path
import sys
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "box86_opcode_catalog", ROOT / "tools/box86_opcode_catalog.py")
catalog = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(catalog)


class OpcodeCatalogTests(unittest.TestCase):
    def test_literal_cases_ignore_comments(self):
        values = catalog.source_case_values("""
            /* case 0x01: */
            switch (opcode) { case 0x0F: break; // case 0xFE:
                switch ((modrm >> 3) & 7) { case 0x03: break; }
            }
        """)
        self.assertEqual(set(values), {15, 3})
        self.assertTrue(all(label.startswith("case@") for labels in values.values()
                            for label in labels))

    def test_primary_family_macros_expand(self):
        values = catalog.source_case_values(
            "GO(0x00, add)\nGOCOND(0x70,\n  anything\n)", primary=True)
        self.assertEqual([values[index][0].split("+")[-1]
                          for index in range(0x00, 0x06)],
                         [str(index) for index in range(6)])
        self.assertEqual([values[index][0].split("+")[-1]
                          for index in range(0x70, 0x80)],
                         [str(index) for index in range(16)])

    def test_probe_rows_validate_and_sort_deterministically(self):
        probe = """{"schema":1,"probe":"representative-decode-only","rows":[
          {"prefix":"f3","map":"0f","opcode":16,"reg_candidate_mask":3,"mem_candidate_mask":1},
          {"prefix":"none","map":"primary","opcode":137,"reg_candidate_mask":255,"mem_candidate_mask":255}]}"""
        parsed = catalog.parse_probe_json(probe)
        self.assertEqual([row["opcode"] for row in parsed["rows"]], [16, 137])
        with self.assertRaises(ValueError):
            catalog.parse_probe_json('{"schema":9,"probe":"wrong","rows":[]}')

    def test_maps_keep_interpreter_and_arm_sources_separate(self):
        primary_interpreter, primary_arm = catalog.SOURCE_MAPS["primary"]
        self.assertEqual(primary_interpreter, ("src/emu/x86run.c",))
        self.assertEqual(primary_arm, ("src/dynarec/dynarec_arm_00.c",))
        self.assertIn("x87-d8", catalog.SOURCE_MAPS)
        self.assertIn("66f20f", catalog.SOURCE_MAPS)

    def test_compact_row_json_round_trips(self):
        value = {"source": {"rows": [{"opcode": 1, "labels": ["case@2"]}]},
                 "probe": {"rows": []}}
        rendered = catalog.render_json(value)
        self.assertEqual(__import__("json").loads(rendered), value)
        self.assertIn('{"labels":["case@2"],"opcode":1}', rendered)


if __name__ == "__main__":
    unittest.main()
