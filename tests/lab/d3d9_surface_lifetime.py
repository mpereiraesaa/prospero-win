#!/usr/bin/env python3
"""Native versus production bridge borrowed implicit-surface lifetime proof."""
import argparse,hashlib,json,os,subprocess,time
from pathlib import Path
ap=argparse.ArgumentParser();ap.add_argument('--recovery',type=Path,required=True);ap.add_argument('--output',type=Path,required=True);ap.add_argument('--expect-proxy-failure',action='store_true');ap.add_argument('--production-root',type=Path);ap.add_argument('--owner-control',action='store_true');a=ap.parse_args()
root=(a.production_root or Path(__file__).resolve().parents[2]).resolve();base=a.recovery.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
fixture=Path(__file__).resolve().with_suffix('.c');adapter=fixture.with_name('d3d9_surface_lifetime_device.c');control=fixture.with_name('d3d9_surface_owners_control.c');prefix=out/'prefix';backend=base/'backend/d3d9.dll'
r={'status':'running','console_accessed':False,'test_only_window_adapter':True,'expected_unfixed_bridge_failure':a.expect_proxy_failure,'sources':{str(p.resolve()):sha(p) for p in [*sorted((root/'wine/ps5').rglob('*.c')),*sorted((root/'wine/ps5').rglob('*.h')),fixture,adapter,control,Path(__file__).resolve()]},'commands':[],'backend_sha256':sha(backend)}
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
 # Frozen full production build recipe, with only native HWND adapter replaced.
 recipe=json.loads((base/'production-guest-fullscreen-r1/receipt.json').read_text())
 for build in recipe['builds']:
  cmd=[]
  for arg in build['command']:
   if '/wine/ps5/' in arg:arg=str(root/'wine/ps5'/arg.split('/wine/ps5/',1)[1])
   if arg.endswith('/wine/ps5/d3d9/pw_d3d9_native_device.c'):arg=str(adapter)
   if arg.endswith('/'+build['file']):arg=str(out/build['file'])
   cmd.append(arg)
  cmd[1:1]=['-I'+str(root/'wine/ps5/d3d9')]
  implicit=root/'wine/ps5/pw_d3d9_implicit_wire.c'
  if implicit.exists():
   if str(implicit) not in cmd:cmd.insert(1,str(implicit))
   if '-DPW_D3D9_ENABLE_IMPLICIT' not in cmd:cmd.insert(1,'-DPW_D3D9_ENABLE_IMPLICIT')
  run(cmd,'build-'+build['file'])
 for arch in ('x86_64','i686'):
  run([arch+'-w64-mingw32-gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-municode',fixture,'-luser32','-o',out/(arch+'.exe')],'build-'+arch)
 env=os.environ.copy();env.update(WINEPREFIX=str(prefix),WINEDEBUG='-all',WINEDLLOVERRIDES='mscoree,mshtml=;d3d9=n',DXVK_LOG_PATH=str(out));r['environment']={k:env.get(k) for k in ('DISPLAY','WINEPREFIX','WINEDEBUG','WINEDLLOVERRIDES','DXVK_NO_VR','VK_ICD_FILENAMES')};save()
 if a.owner_control:
  for arch in ('i686','x86_64'):
   sources=[root/'wine/ps5'/n for n in ['d3d9/pw_d3d9_texture_proxy.c','d3d9/pw_d3d9_texture_client.c','d3d9/pw_d3d9_staging.c','d3d9/pw_d3d9_private_data.c','pw_d3d9_texture_wire.c']]
   run([arch+'-w64-mingw32-gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-array-bounds','-Wno-misleading-indentation','-I'+str(root/'wine/ps5/d3d9'),control,*sources,'-luuid','-ldxguid','-o',out/(arch+'-control.exe')],'build-'+arch+'-control')
   run([base/'host/build/loader/wine',out/(arch+'-control.exe')],arch+'-control',env)
 run([base/'host/build/loader/wine',out/'x86_64.exe',win(backend)],'native',env)
 run([base/'host/build/loader/wine',out/'i686.exe',win(out/'d3d9.dll'),win(out/'service.dll'),win(backend),*(['--expect-proxy-failure'] if a.expect_proxy_failure else [])],'bridge',env)
 for label in ('native','bridge'):
  s=(out/(label+'.stdout')).read_text();assert s.count('SURFACE_LIFETIME ')==6 and 'SURFACE_CLOSE status=0' in s
  if label=='native' or not a.expect_proxy_failure:assert s.count('SURFACE_RESET ')==3
  if label=='bridge' and not a.expect_proxy_failure:assert s.count('SURFACE_MIRROR ')==3
 assert all(sha(root/p)==h for p,h in r['sources'].items())
 r['artifacts']={p.name:sha(p) for p in out.iterdir() if p.suffix in ('.dll','.exe')};r['status']='pass'
except BaseException as e:
 if r['status']!='timeout':r['status']='failed'
 r['error']=repr(e);save();raise
save();print(out/'receipt.json')
