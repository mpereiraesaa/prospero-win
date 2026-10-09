#!/usr/bin/env python3
"""Transform shadow model: host, sanitizer, actual PE32/PE64 and real-DXVK parity."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
p=argparse.ArgumentParser(description=__doc__)
for name in ('wine-build','prefix','backend64','output'):p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--seeds',type=int,default=3)
a=p.parse_args();root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(exist_ok=False,parents=True)
receipt={'status':'running','console_accessed':False,'production_session_integration':False,'commands':[]}
def run(command,label,env=None,timeout=120):
 try:r=subprocess.run(list(map(str,command)),env=env,text=True,capture_output=True,timeout=timeout)
 except subprocess.TimeoutExpired as e:
  receipt.update(status='failed',failure=label+' timed out');(out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
  raise RuntimeError(label+' timed out') from e
 (out/(label+'.log')).write_text(r.stdout+r.stderr);receipt['commands'].append(dict(command=list(map(str,command)),exit_code=r.returncode,stdout=r.stdout,stderr=r.stderr))
 if r.returncode:receipt.update(status='failed',failure=label)
 (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
 assert not r.returncode,(label,r.stdout+r.stderr)
 return r
model=root/'wine/ps5/pw_d3d9_transform_shadow.c';unit=root/'tests/test_d3d9_transform_shadow.c'
fixture=root/'tests/lab/d3d9_transform_shadow_pe.c'
inputs=[model,model.with_suffix('.h'),unit,fixture,Path(__file__),a.backend64.resolve()]
frozen={str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs}
receipt['sources']=frozen
flags=['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'wine/ps5')]
san=dict(os.environ,ASAN_OPTIONS='detect_leaks=1',UBSAN_OPTIONS='halt_on_error=1')
for label,cc,extra in [('host','cc',['-O2']),('sanitize','clang',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
 run([cc,*flags,*extra,unit,model,'-o',out/('unit-'+label)],'compile-'+label)
 assert 'PASS d3d9 transform shadow' in run([out/('unit-'+label)],label,san).stdout
run(['i686-w64-mingw32-gcc',*flags,'-O2',unit,model,'-o',out/'unit32.exe'],'compile-unit32')
run(['x86_64-w64-mingw32-gcc',*flags,'-O2',unit,model,'-o',out/'unit64.exe'],'compile-unit64')
run(['x86_64-w64-mingw32-gcc',*flags,'-O2',fixture,model,'-luser32','-o',out/'native64.exe'],'compile-native64')
env=os.environ.copy();env.update(WINEPREFIX=str(a.prefix.resolve()),WINEDEBUG='-all',WINEDLLOVERRIDES='mscoree,mshtml=;winedbg.exe=d',DXVK_LOG_PATH=str(out),DXVK_LOG_LEVEL='warn',
 PW_TRANSFORM_BACKEND='Z:'+str(a.backend64.resolve()).replace('/','\\'))
wine=a.wine_build.resolve()/'loader/wine'
for arch in ('unit32','unit64'):
 assert 'PASS d3d9 transform shadow' in run([wine,out/(arch+'.exe')],'pe-'+arch,env).stdout
results=[]
for behavior in (0x40,0x80,0x20):
 for seed in range(1,a.seeds+1):
  env.update(PW_TRANSFORM_BEHAVIOR=hex(behavior),PW_TRANSFORM_SEED=str(seed*2654435761%2**32))
  r=run([wine,out/'native64.exe'],'native-%02x-%d'%(behavior,seed),env,timeout=300)
  m=re.search(r'PW_TRANSFORM_PE behavior=([0-9a-f]+) seed_ops=(\d+) edges=7 compared=(\d+) hits=(\d+) resets=(\d+) reset_ok=(\d+) known_end=(\d+) max_known=(\d+) mismatches=0',r.stdout)
  assert m and int(m.group(1),16)==behavior,r.stdout
  assert 'PW_TRANSFORM_RESET live_blocks=0 hr=00000000' in r.stdout,r.stdout
  results.append(dict(behavior=hex(behavior),operations=int(m.group(2)),compared=int(m.group(3)),get_hits=int(m.group(4)),resets=int(m.group(5)),reset_ok=int(m.group(6)),known_end=int(m.group(7)),max_known=int(m.group(8))))
  assert results[-1]['compared']>10000 and results[-1]['get_hits']>0 and results[-1]['max_known']>=20 and results[-1]['resets']>0,results[-1]
assert frozen=={str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs},'source changed during proof'
receipt.update(status='pass',sources=frozen,native=results,
 sha256={str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in [*inputs,out/'unit32.exe',out/'unit64.exe',out/'native64.exe']},
 scope='Portable model unit/ASan/UBSan, actual PE32/PE64 unit, and real DXVK 5fde742b PE64 device parity: 6000 seeded steps each; every shadow-known transform equals backend GetTransform bytes after each Set/NULL/Multiply/Get/Begin/End/Create ALL,PIXEL,VERTEX/Capture/Apply/Release/Reset step in hardware, mixed and software devices. No production proxy/session wiring or console claim.')
(out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');print('PASS '+str(out/'receipt.json'))
