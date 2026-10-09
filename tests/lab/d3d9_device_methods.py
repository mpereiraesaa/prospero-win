#!/usr/bin/env python3
"""Compile and run all supported typed device slots on host, PE32 and PE64."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
for n in ('wine-source','wine-build','prefix','output'):p.add_argument('--'+n,type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
native=root/'wine/ps5/d3d9/pw_d3d9_device_methods.c';fixture=root/'tests/lab/d3d9_device_methods.c'
codecs=[root/'wine/ps5/pw_d3d9_command_wire.c',root/'wine/ps5/pw_d3d9_getter_wire.c']
inputs=[native,native.with_suffix('.h'),fixture,Path(__file__),*codecs]
frozen={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
r={'status':'running','console_accessed':False,'production_session_integration':False,'commands':[]}
def run(cmd,label,env=None):
 x=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,env=env,timeout=90)
 (out/(label+'.log')).write_text(x.stdout+x.stderr);r['commands'].append({'command':list(map(str,cmd)),'exit_code':x.returncode,'stdout':x.stdout,'stderr':x.stderr})
 (out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n');assert x.returncode==0,(label,x.stdout+x.stderr)
 return x
flags=['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'wine/ps5/d3d9')]
for label,extra in [('host',[]),('sanitize',['-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
 run(['cc',*flags,*extra,'-D_WIN64','-D__WINESRC__','-I'+str(a.wine_build.resolve()/'include'),'-I'+str(a.wine_source.resolve()/'include'),fixture,native,*codecs,'-o',out/label],'compile-'+label)
 run([out/label],label)
a.prefix.resolve().mkdir(parents=True,exist_ok=True)
env=os.environ.copy();env.update(WINEPREFIX=str(a.prefix.resolve()),WINEDEBUG='-all',WINEDLLOVERRIDES='mscoree,mshtml=')
for bits,compiler in [(32,'i686-w64-mingw32-gcc'),(64,'x86_64-w64-mingw32-gcc')]:
 name='pe'+str(bits);exe=out/(name+'.exe')
 run([compiler,*flags,'-O2',fixture,native,*codecs,'-o',exe],'compile-'+name)
 x=run([a.wine_build.resolve()/'loader/wine',exe],name,env)
 assert 'PASS typed device methods:40 command+28 getter slots' in x.stdout
assert frozen=={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
r.update(status='pass',command_methods=40,getter_methods=28,sha256=frozen,scope='All typed slots and payloads, required pointers, zero/bounded arrays, object resolution failure, HRESULT and atomic malformed-reply outputs. Real PE32/PE64 executables use controlled callbacks; no native backend/session/console claim.')
(out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n');print('PASS '+str(out/'receipt.json'))
