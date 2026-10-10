#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Build/run the portable stream with sanitizers; compile its actual x86 producer."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = root / 'wine/ps5/pw_vk_command_stream.c'
fixture = root / 'tests/test_vk_command_stream.c'
cc = shutil.which('cc')
x86 = shutil.which('i686-w64-mingw32-gcc')
assert cc and x86, 'Host C and i686 MinGW compilers are required'
output = args.output or Path(tempfile.mkdtemp(prefix='pw-vk-stream-'))
output.mkdir(parents=True, exist_ok=True)
common = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(source.parent)]
commands = []
for name, flags in [('host', ['-O2']), ('sanitized', ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'])]:
    cmd = [cc, *common, *flags, str(source), str(fixture), '-pthread', '-o', str(output / name)]
    commands.append(cmd)
    subprocess.run(cmd, check=True)
    subprocess.run([str(output / name)], check=True)
cmd = [x86, *common, '-O2', '-c', str(source), '-o', str(output / 'producer-x86.o')]
commands.append(cmd)
subprocess.run(cmd, check=True)
receipt = {'host_pass': True, 'asan_ubsan_pass': True, 'actual_x86_compile_pass': True,
           'console_verified': False, 'wine_adapter_integrated': False,
           'commands': commands,
           'sha256': {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in [source, source.with_suffix('.h'), fixture, Path(__file__).resolve()]}}
(output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print('Receipt:', output / 'receipt.json')
