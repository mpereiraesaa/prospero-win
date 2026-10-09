#!/usr/bin/env python3
"""Native versus production bridge borrowed implicit-surface lifetime proof."""
import argparse,hashlib,json,os,subprocess,time
from pathlib import Path
ap=argparse.ArgumentParser();ap.add_argument('--recovery',type=Path,required=True);ap.add_argument('--output',type=Path,required=True);ap.add_argument('--expect-proxy-failure',action='store_true');ap.add_argument('--production-root',type=Path);ap.add_argument('--owner-control',action='store_true');a=ap.parse_args()
root=(a.production_root or Path(__file__).resolve().parents[2]).resolve();base=a.recovery.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
fixture=Path(__file__).resolve().with_name('d3d9_surface_lifetime.c');adapter=fixture.with_name('d3d9_surface_lifetime_device.c');control=fixture.with_name('d3d9_surface_owners_control.c');prefix=out/'prefix';backend=base/'backend/d3d9.dll'
r={'status':'running','console_accessed':False,'test_only_window_adapter':True,'async_modes':['0','1'],'sources':{str(p.resolve()):sha(p) for p in [*sorted((root/'wine/ps5').rglob('*.c')),*sorted((root/'wine/ps5').rglob('*.h')),fixture,adapter,control,root/'tests/lab/d3d9_game_build.py',Path(__file__).resolve()]},'commands':[],'backend_sha256':sha(backend)}
def save():(out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n')
def run(cmd,label,env=None):
 cmd=list(map(str,cmd));entry={'command':cmd,'label':label};r['commands'].append(entry);save()
 with (out/(label+'.stdout')).open('w') as o,(out/(label+'.stderr')).open('w') as e:
  child=subprocess.Popen(cmd,stdout=o,stderr=e,env=env);entry['pid']=child.pid;save()
  try:code=child.wait(timeout=90)
  except subprocess.TimeoutExpired:
   child.kill();child.wait();entry.update(status='timeout',exit_code=child.returncode);r['status']='timeout';save();raise
 entry.update(status='complete',exit_code=code);save()
 if code:raise RuntimeError((label,code))
def win(p):return 'Z:'+str(p.resolve()).replace('/',chr(92))
try:
 run(['cp','-a','--reflink=auto',base/'host/prefix',prefix],'prefix-copy')
 # Current shipping recipe, then replace only the host HWND adapter.
 run(['python3',root/'tests/lab/d3d9_game_build.py','--api-diagnostics','--output',out/'pair'],'pair-build')
 recipe=json.loads((out/'pair/receipt.json').read_text())
 for build in recipe['builds']:
  if build['file']=='d3d9.dll':
   run(['cp',out/'pair/d3d9.dll',out/'d3d9.dll'],'copy-proxy');continue
  cmd=[str(adapter) if arg.endswith('/d3d9/pw_d3d9_native_device.c') else arg for arg in build['command']]
  cmd.insert(1,'-I'+str(root/'wine/ps5/d3d9'));cmd[cmd.index('-o')+1]=str(out/'service.dll')
  run(cmd,'build-host-service')
 for arch in ('x86_64','i686'):
  run([arch+'-w64-mingw32-gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-municode',fixture,'-luser32','-o',out/(arch+'.exe')],'build-'+arch)
 env=os.environ.copy();env.update(WINEPREFIX=str(prefix),WINEDEBUG='-all',WINEDLLOVERRIDES='mscoree,mshtml=;d3d9=n',DXVK_LOG_PATH=str(out));r['environment']={k:env.get(k) for k in ('DISPLAY','WINEPREFIX','WINEDEBUG','WINEDLLOVERRIDES','DXVK_NO_VR','VK_ICD_FILENAMES')};save()
 if a.owner_control:
  for arch in ('i686','x86_64'):
   sources=[root/'wine/ps5'/n for n in ['d3d9/pw_d3d9_texture_proxy.c','d3d9/pw_d3d9_texture_client.c','d3d9/pw_d3d9_staging.c','d3d9/pw_d3d9_private_data.c','pw_d3d9_texture_wire.c']]
   run([arch+'-w64-mingw32-gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-array-bounds','-Wno-misleading-indentation','-I'+str(root/'wine/ps5/d3d9'),control,*sources,'-luuid','-ldxguid','-o',out/(arch+'-control.exe')],'build-'+arch+'-control')
   run([base/'host/build/loader/wine',out/(arch+'-control.exe')],arch+'-control',env)
 run([base/'host/build/loader/wine',out/'x86_64.exe',win(backend)],'native',env)
 for mode in ('0','1'):
  env['PW_D3D9_ASYNC']=mode
  run([base/'host/build/loader/wine',out/'i686.exe',win(out/'d3d9.dll'),win(out/'service.dll'),win(backend)],'bridge-'+mode,env)
 for label in ('native','bridge-0','bridge-1'):
  text=(out/(label+'.stdout')).read_text();assert text.count('SURFACE_LIFETIME ')==6 and 'SURFACE_CLOSE status=0' in text
  assert text.count('SURFACE_RESET ')==3
  if label!='native':assert text.count('SURFACE_MIRROR ')==3
 assert all(sha(root/p)==h for p,h in r['sources'].items())
 r['artifacts']={p.name:sha(p) for p in out.iterdir() if p.suffix in ('.dll','.exe')};r['status']='pass'
except BaseException as e:
 if r['status']!='timeout':r['status']='failed'
 r['error']=repr(e);save();raise
save();print(out/'receipt.json')
