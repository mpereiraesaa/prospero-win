#!/usr/bin/env python3
"""Run actual PE32/PE64 texture proxy lifetime and typed ABI checks."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
for n in ('wine-build','prefix','output'):p.add_argument('--'+n,type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False);a.prefix.resolve().mkdir(parents=True,exist_ok=True)
files=[root/'tests/lab/d3d9_texture_proxy.c',root/'wine/ps5/d3d9/pw_d3d9_texture_proxy.c',root/'wine/ps5/pw_d3d9_texture_wire.c',root/'wine/ps5/d3d9/pw_d3d9_texture_client.c',root/'wine/ps5/d3d9/pw_d3d9_staging.c',root/'wine/ps5/d3d9/pw_d3d9_private_data.c']
inputs=[*files,Path(__file__),*sorted((root/'wine/ps5').rglob('*.h'))];frozen={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
r={'status':'running','console_accessed':False,'production_session_integration':False,'commands':[]}
def run(cmd,label,env=None):
 x=subprocess.run(list(map(str,cmd)),text=True,capture_output=True,env=env,timeout=90);(out/(label+'.log')).write_text(x.stdout+x.stderr)
 r['commands'].append({'command':list(map(str,cmd)),'exit_code':x.returncode,'stdout':x.stdout,'stderr':x.stderr});(out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n');assert x.returncode==0,(label,x.stdout+x.stderr);return x
flags=['-std=gnu11','-Wall','-Wextra','-Werror','-O2','-I'+str(root/'wine/ps5/d3d9')]
env=os.environ.copy();env.update(WINEPREFIX=str(a.prefix.resolve()),WINEDEBUG='-all',WINEDLLOVERRIDES='mscoree,mshtml=')
for bits,cc in [(32,'i686-w64-mingw32-gcc'),(64,'x86_64-w64-mingw32-gcc')]:
 name='pe'+str(bits);exe=out/(name+'.exe');run([cc,*flags,*files,'-ldxguid','-o',exe],'compile-'+name)
 assert 'PASS texture proxy:' in run([a.wine_build.resolve()/'loader/wine',exe],name,env).stdout
assert frozen=={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
r.update(status='pass',sha256=frozen,artifacts={str(out/('pe'+str(bits)+'.exe')):hashlib.sha256((out/('pe'+str(bits)+'.exe')).read_bytes()).hexdigest() for bits in (32,64)},scope='Actual PE32/PE64 COM ABI; immutable texture description hits avoid RPC, failures remain uncached and surfaces remain synchronous; PE32 staging success and PE64 high-pointer allocation rejection, canonical identity, strong parent, signed low32 staging, RT/depth/stretch, failure and deferred locked release ownership with controlled callbacks; no native session/backend claim.')
(out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n');print('PASS '+str(out/'receipt.json'))
