#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
import importlib.util
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'tools/generate_d3d9_inventory.py'
sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location('inventory', SCRIPT)
inventory = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(inventory)

class Inventory(unittest.TestCase):
    def test_parser(self):
        source = '''DECLARE_INTERFACE_IID_(IDirect3D9,IUnknown,"example")
{
    /* names and pointer types are preserved */
    STDMETHOD_(HRESULT,QueryInterface)(THIS_ REFIID riid, void** out) PURE;
    STDMETHOD_(ULONG,AddRef)(THIS) PURE;
    STDMETHOD_(ULONG,Release)(THIS) PURE;
};'''
        methods = inventory.parse(source)['IDirect3D9']
        self.assertEqual(methods, [('QueryInterface', 'HRESULT', 'REFIID riid, void** out'),
                                   ('AddRef', 'ULONG', ''), ('Release', 'ULONG', '')])
        with self.assertRaises(ValueError):
            inventory.parse(source + '\n' + source)
        with self.assertRaises(ValueError):
            inventory.parse(source.replace('THIS_ REFIID', 'REFIID'))
        with self.assertRaises(ValueError):
            inventory.parse(source.replace('};', 'int unparsed;\n};'))
        with self.assertRaises(ValueError):
            inventory.parse(source.replace('AddRef', 'Release'))
        with self.assertRaises(ValueError):
            inventory.render({'IDirect3D9': methods})

    def test_checked_inventory(self):
        text = (ROOT / 'wine/ps5/d3d9/pw_d3d9_inventory.h').read_text()
        self.assertIn(inventory.HEADER_SHA256, text)
        parsed = {}
        policies = []
        for iface, slot, name, returned, params, policy in re.findall(
                r'^    X\((IDirect\w+), (\d+), (\w+), (\w+), \((.*?)\), (PW_D3D9_POLICY_\w+)\)', text, re.M):
            methods = parsed.setdefault(iface, [])
            self.assertEqual(int(slot), len(methods))
            self.assertEqual(policy, inventory.policy(iface, int(slot)))
            prefix = iface + ' *self'
            self.assertTrue(params.startswith(prefix))
            args = params[len(prefix):]
            self.assertTrue(not args or args.startswith(', '))
            methods.append((name, returned, args[2:] if args else ''))
            policies.append(policy)
        self.assertEqual({k: len(v) for k, v in parsed.items()}, inventory.COUNTS)
        self.assertEqual(text, inventory.render(parsed))
        self.assertEqual(policies.count('PW_D3D9_POLICY_LOCAL_IDENTITY'), 51)
        self.assertEqual(policies.count('PW_D3D9_POLICY_SYNC_FACTORY'), 11)
        device = parsed['IDirect3DDevice9']
        self.assertEqual(device[16][0], 'Reset')
        self.assertEqual(device[17][0], 'Present')
        self.assertEqual(device[-1][0], 'CreateQuery')
        self.assertEqual(inventory.policy('IDirect3DDevice9', 17), 'PW_D3D9_POLICY_UNSUPPORTED')

    def test_unpinned_header_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'header.h').write_text('unreviewed input')
            (root / 'output.h').write_text('unchanged')
            result = subprocess.run([sys.executable, str(SCRIPT), '--header', str(root / 'header.h'),
                                     '--output', str(root / 'output.h')], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('pinned Wine', result.stderr)
            self.assertEqual((root / 'output.h').read_text(), 'unchanged')

if __name__ == '__main__':
    unittest.main()
