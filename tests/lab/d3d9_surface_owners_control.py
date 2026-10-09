#!/usr/bin/env python3
"""Actual PE32/PE64 controlled implicit-owner lifecycle, without a GPU."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser()
for name in ('production-root','recovery','output'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();root=a.production_root.resolve();base=a.recovery.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
fixture=Path(__file__).resolve().with_suffix('.c');prefix=out/'prefix';sha=lambda x:hashlib.sha256(x.read_bytes()).hexdigest()
sources=[root/'wine/ps5'/n for n in ['d3d9/pw_d3d9_texture_proxy.c','d3d9/pw_d3d9_texture_client.c','d3d9/pw_d3d9_staging.c','d3d9/pw_d3d9_private_data.c','pw_d3d9_texture_wire.c']]
r={'status':'running','d3d_backend_loaded':False,'console_accessed':False,'sources':{str(x):sha(x) for x in [*sources,*sorted((root/'wine/ps5').rglob('*.h')),fixture,Path(__file__).resolve()]},'commands':[]}
def save():(out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n')
def run(cmd,label,env=None):
 cmd=list(map(str,cmd));entry={'command':cmd,'label':label};r['commands'].append(entry);save()
 with (out/(label+'.stdout')).open('w') as stdout,(out/(label+'.stderr')).open('w') as stderr:
  child=subprocess.Popen(cmd,stdout=stdout,stderr=stderr,env=env);entry['pid']=child.pid;save()
  try:code=child.wait(timeout=60)
  except subprocess.TimeoutExpired:
   child.kill();child.wait();entry.update(status='timeout',exit_code=child.returncode);r['status']='timeout';save();raise
 entry.update(status='complete',exit_code=code);save()
 if code:raise RuntimeError((label,code))
try:
 run(['cp','-a','--reflink=auto',base/'host/prefix',prefix],'prefix')
 env=os.environ.copy();env.update(WINEPREFIX=str(prefix),WINEDEBUG='-all',WINEDLLOVERRIDES='mscoree,mshtml=')
 for arch in ('i686','x86_64'):
  run([arch+'-w64-mingw32-gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-array-bounds','-Wno-misleading-indentation','-I'+str(root/'wine/ps5/d3d9'),fixture,*sources,'-luuid','-ldxguid','-o',out/(arch+'.exe')],arch+'-build')
  run([base/'host/build/loader/wine',out/(arch+'.exe')],arch,env)
  assert 'SURFACE_OWNERS_CONTROL frozen_addref=0 early_identity=1 retire=1 parent_cycle=0 status=0' in (out/(arch+'.stdout')).read_text()
 assert all(sha(Path(path))==h for path,h in r['sources'].items())
 r['artifacts']={x.name:sha(x) for x in out.glob('*.exe')};r['status']='pass'
except BaseException as e:
 if r['status']!='timeout':r['status']='failed'
 r['error']=repr(e);save();raise
save();print(out/'receipt.json')
