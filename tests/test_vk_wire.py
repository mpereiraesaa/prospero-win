#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Validate normalized Vulkan payloads and template metadata independently of Wine."""
import argparse, hashlib, json, shutil, subprocess, tempfile
from pathlib import Path
parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path);args=parser.parse_args()
root=Path(__file__).resolve().parents[1];base=root/'wine/ps5'
sources=[base/'pw_vk_wire.c',base/'pw_vk_template_cache.c'];fixture=root/'tests/test_vk_wire.c'
cc=shutil.which('cc');x86=shutil.which('i686-w64-mingw32-gcc');assert cc and x86
out=args.output or Path(tempfile.mkdtemp(prefix='pw-vk-wire-'));out.mkdir(parents=True,exist_ok=True)
flags=['-std=c11','-Wall','-Wextra','-Werror','-I'+str(base)];commands=[]
for name,extra in [('host',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
 cmd=[cc,*flags,*extra,*map(str,sources),str(fixture),'-o',str(out/name)];commands.append(cmd)
 subprocess.run(cmd,check=True);subprocess.run([str(out/name)],check=True)
for src in sources:
 cmd=[x86,*flags,'-O2','-c',str(src),'-o',str(out/(src.stem+'-x86.o'))];commands.append(cmd);subprocess.run(cmd,check=True)
files=[*sources,base/'pw_vk_wire.h',base/'pw_vk_template_cache.h',fixture,Path(__file__).resolve()]
receipt={'host_pass':True,'asan_ubsan_pass':True,'x86_compile_pass':True,'wine_runtime_integrated':False,'console_verified':False,'commands':commands,'sha256':{str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest() for f in files}}
(out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');print('Receipt:',out/'receipt.json')
