#!/usr/bin/env python3
"""Forty-method native ABI plus PE32-owned payloads on a real PE64 DXVK device."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import uuid
p=argparse.ArgumentParser(description=__doc__)
for name in ('wine-source','wine-build','prefix','backend64','output'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(exist_ok=False,parents=True)
receipt={'status':'running','console_accessed':False,'production_session_integration':False,'commands':[]}
def run(command,label,env=None):
 r=subprocess.run(list(map(str,command)),env=env,text=True,capture_output=True,timeout=60)
 (out/(label+'.log')).write_text(r.stdout+r.stderr);receipt['commands'].append(dict(command=list(map(str,command)),exit_code=r.returncode,stdout=r.stdout,stderr=r.stderr))
 (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
 assert not r.returncode,(label,r.stdout+r.stderr)
 return r
native=root/'wine/ps5/d3d9/pw_d3d9_native_command.c';codec=root/'wine/ps5/pw_d3d9_command_wire.c'
fixture=root/'tests/lab/d3d9_native_command.c';pe=root/'tests/lab/d3d9_native_command_pe.c'
includes=['-I'+str(root/'wine/ps5'),'-I'+str(root/'wine/ps5/d3d9')]
inputs=[native,native.with_suffix('.h'),native.parent/'pw_d3d9_kinds.h',codec,fixture,pe,Path(__file__)]
frozen={str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs}
flags=['-std=gnu11','-Wall','-Wextra','-Werror']
for label,extra in [('host',[]),('sanitize',['-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
 run(['cc',*flags,*extra,'-D_WIN64','-D__WINESRC__','-I'+str(a.wine_build.resolve()/'include'),'-I'+str(a.wine_source.resolve()/'include'),*includes,fixture,native,codec,'-o',out/label],'compile-'+label)
 run([out/label],label)
run(['i686-w64-mingw32-gcc',*flags,'-O2',*includes,pe,codec,'-o',out/'client.exe'],'compile-client')
run(['x86_64-w64-mingw32-gcc',*flags,'-O2','-Wno-array-bounds','-shared','-static-libgcc',*includes,pe,native,codec,'-luser32','-o',out/'service.dll'],'compile-service')
env=os.environ.copy();env.update(WINEPREFIX=str(a.prefix.resolve()),WINEDEBUG='-all',WINEDLLOVERRIDES='mscoree,mshtml=;d3d9=n',DXVK_LOG_PATH=str(out),PW_COMMAND_BACKEND='Z:'+str(a.backend64.resolve()).replace('/','\\'))
for i in range(3):
 env['PW_COMMAND_SESSION']=uuid.uuid4().hex
 r=run([a.wine_build.resolve()/'loader/wine',out/'client.exe','Z:'+str(out/'service.dll').replace('/','\\')],'pe-'+str(i),env)
 assert len(re.findall('PW_COMMAND_NATIVE index=',r.stdout))==48,r.stdout
 assert re.search(r'PW_COMMAND_PE status=00000000 create=00000000 present=00000000 commands=48 pins=6 pixel=[0-9a-f]+ zero=6 error=0',r.stdout),r.stdout
assert frozen=={str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs},'source changed during proof'
receipt.update(status='pass',native_methods=40,pe_processes=3,commands_per_process=48,valid_draws_per_process=2,negative_unbound_draws_per_process=2,zero_count_native_comparisons_per_process=6,
 scope='Controlled exact HRESULT/field/pin ABI; PE32-owned codec payloads decoded by PE64 helper on real DXVK, valid primitive/indexed draws and changed-pixel readback. No production COM proxy/session wiring or console performance claim.',
 sha256={str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in [native,native.with_suffix('.h'),codec,fixture,pe,Path(__file__),a.backend64.resolve(),out/'client.exe',out/'service.dll']})
(out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');print('PASS '+str(out/'receipt.json'))
