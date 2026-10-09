#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Compile and run actual PE32/PE64 gamma COM ABI against controlled transport."""
import argparse, hashlib, json, os, subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
for arg in ('wine-build','prefix','output'):p.add_argument('--'+arg,type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
files=['tests/lab/d3d9_gamma_proxy.c','wine/ps5/d3d9/pw_d3d9_gamma_proxy.c','wine/ps5/pw_d3d9_gamma_wire.c','wine/ps5/d3d9/pw_d3d9_gamma_proxy.h','wine/ps5/pw_d3d9_gamma_wire.h']
def hashes():return {f:hashlib.sha256((root/f).read_bytes()).hexdigest() for f in files}
r={'status':'running','sources':hashes(),'commands':[],'console_accessed':False,'scope':'Actual PE32/PE64 gamma COM ABI with controlled transport; no native backend or session integration claim.'}
env=os.environ.copy();env.update(WINEPREFIX=str(a.prefix.resolve()),WINEDEBUG='-all',WINEDLLOVERRIDES='mscoree,mshtml=')
def run(command,name):
 result=subprocess.run(list(map(str,command)),env=env,capture_output=True,text=True,timeout=120)
 (out/(name+'.log')).write_text(result.stdout+result.stderr);r['commands'].append({'command':list(map(str,command)),'exit':result.returncode});assert result.returncode==0,(name,result.stderr);return result.stdout
try:
 for bits,cc in [(32,'i686-w64-mingw32-gcc'),(64,'x86_64-w64-mingw32-gcc')]:
  exe=out/f'pe{bits}.exe';run([cc,'-std=c11','-O2','-Wall','-Wextra','-Werror',*[root/f for f in files[:3]],'-ldxguid','-o',exe],f'compile{bits}')
  assert 'PW_GAMMA_PROXY PASS' in run([a.wine_build.resolve()/'loader/wine',exe],f'pe{bits}')
 assert r['sources']==hashes();r['status']='pass'
finally:(out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n')
print('PASS '+str(out/'receipt.json'))
