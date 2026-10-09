#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Verify actual old/new pipeline pair rejection before object publication."""
import argparse,hashlib,json,os,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
for name in ('recovery','old-pair','new-pair','prefix','output'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
source=Path(__file__).with_suffix('.c').resolve();backend=a.recovery.resolve()/'backend/d3d9.dll'
r={'status':'running','console_accessed':False,'scope':'Real mixed pair pre-READY rejection; descriptor ring mismatch may reject before HELLO. No factory published.','sources':{str(source):sha(source),str(Path(__file__).resolve()):sha(Path(__file__))},'artifacts':{str(backend):sha(backend)},'commands':[]}
def save():(out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n')
def run(label,cmd,env=None):
 cmd=list(map(str,cmd));entry={'command':cmd};r['commands'].append(entry);save()
 try:x=subprocess.run(cmd,env=env,capture_output=True,text=True,timeout=90)
 except subprocess.TimeoutExpired:entry['status']='timeout';save();raise
 for name in ('stdout','stderr'):(out/(label+'.'+name)).write_text(getattr(x,name))
 entry['exit_code']=x.returncode;save();assert x.returncode==0,(label,x.stderr)
 return x.stdout+x.stderr
win=lambda p:'Z:'+str(p.resolve()).replace('/',chr(92))
try:
 for pair,features in ((a.old_pair,260095),(a.new_pair,522239)):
  receipt=json.loads((pair/'receipt.json').read_text());assert receipt['features']==features
  r['artifacts'][str(pair/'receipt.json')]=sha(pair/'receipt.json')
  for name in ('d3d9.dll','service.dll'):
   assert sha(pair/name)==receipt[name];r['artifacts'][str(pair/name)]=sha(pair/name)
 assert not a.prefix.exists(),'prefix must be isolated and new'
 run('prefix',['cp','-a','--reflink=auto',a.recovery/'host/prefix',a.prefix.resolve()])
 run('build',['i686-w64-mingw32-gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-municode',source,'-o',out/'mixed.exe'])
 r['artifacts'][str(out/'mixed.exe')]=sha(out/'mixed.exe')
 env=os.environ.copy();env.update(WINEPREFIX=str(a.prefix.resolve()),WINEDEBUG='-all',WINEDLLOVERRIDES='mscoree,mshtml=;d3d9=n;winedbg.exe=d',PW_D3D9_ASYNC='1',PW_D3D9_PROFILE='1',PW_D3D9_DIAGNOSTICS='0')
 for label,proxy,service in (('old-new',a.old_pair/'d3d9.dll',a.new_pair/'service.dll'),('new-old',a.new_pair/'d3d9.dll',a.old_pair/'service.dll')):
  log=run(label,[a.recovery/'host/build/loader/wine',out/'mixed.exe',win(proxy),win(service),win(backend)],env)
  assert 'PW_PIPELINE_MIXED factory_null=1 no_published_object=1 status=0' in log
  phases=re.findall(r'PW_D3D9 service opcode=(\d+) phase=(\w+) error=(\d+)',log)
  assert phases and all(op=='0' and phase=='startup' for op,phase,error in phases),phases
  r[label]={'rejection_phase':phases,'published_factory':False}
 assert all(sha(Path(path))==digest for path,digest in r['sources'].items())
 r['logs']={str(p):sha(p) for p in out.iterdir() if p.suffix in ('.stdout','.stderr')};r['status']='pass'
except BaseException as e:r.update(status='failed',error=repr(e));raise
finally:save()
print(out/'receipt.json')
