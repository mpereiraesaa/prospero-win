#!/usr/bin/env python3
"""Run actual PE32/PE64 typed cursor frontend/native ABI checks."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
for n in ('wine-build','prefix','backend64','output'):p.add_argument('--'+n,type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False);a.prefix.resolve().mkdir(parents=True,exist_ok=True)
files=[root/'tests/lab/d3d9_cursor.c',root/'wine/ps5/d3d9/pw_d3d9_cursor.c',root/'wine/ps5/pw_d3d9_cursor_wire.c']
pe=root/'tests/lab/d3d9_cursor_pe.c';inputs=[*files,files[1].with_suffix('.h'),pe,Path(__file__)];frozen={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
r={'status':'running','console_accessed':False,'production_session_integration':False,'commands':[]}
def run(cmd,label,env=None):
 x=subprocess.run(list(map(str,cmd)),text=True,capture_output=True,env=env,timeout=90);(out/(label+'.log')).write_text(x.stdout+x.stderr)
 r['commands'].append({'command':list(map(str,cmd)),'exit_code':x.returncode,'stdout':x.stdout,'stderr':x.stderr});(out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n');assert x.returncode==0,(label,x.stdout+x.stderr);return x
flags=['-std=gnu11','-Wall','-Wextra','-Werror','-O2','-I'+str(root/'wine/ps5/d3d9')]
env=os.environ.copy();env.update(WINEPREFIX=str(a.prefix.resolve()),WINEDEBUG='-all',WINEDLLOVERRIDES='mscoree,mshtml=')
for bits,cc in [(32,'i686-w64-mingw32-gcc'),(64,'x86_64-w64-mingw32-gcc')]:
 name='pe'+str(bits);exe=out/(name+'.exe');run([cc,*flags,*files,'-ldxguid','-o',exe],'compile-'+name)
 assert 'PASS cursor adapters:' in run([a.wine_build.resolve()/'loader/wine',exe],name,env).stdout
run(['i686-w64-mingw32-gcc',*flags,pe,'-o',out/'native-client.exe'],'compile-native-client')
run(['x86_64-w64-mingw32-gcc',*flags,'-Wno-array-bounds','-shared','-static-libgcc',pe,*files[1:],'-luser32','-o',out/'service.dll'],'compile-native-service')
def win(p):return 'Z:'+str(p.resolve()).replace('/',chr(92))
env.update(PW_CURSOR_BACKEND=win(a.backend64),DXVK_LOG_PATH=str(out),WINEDLLOVERRIDES='mscoree,mshtml=;d3d9=n')
for cycle in range(3):
 assert 'PW_CURSOR_PE status=00000000 create=00000000 checks=4 error=0' in run([a.wine_build.resolve()/'loader/wine',out/'native-client.exe',win(out/'service.dll')],'native-'+str(cycle),env).stdout
assert frozen=={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
r.update(status='pass',sha256=frozen,scope='Actual PE32/PE64 COM ABI, owned surface acquisition, exact signed coordinate/BOOL bits and HRESULT, actual void dispatch, sticky void/value failures and reentrant device release. Controlled ABI plus three actual native DXVK processes comparing cursor properties/null HRESULT, real position dispatch and ShowCursor results. No production session or console claim.')
(out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n');print('PASS '+str(out/'receipt.json'))
