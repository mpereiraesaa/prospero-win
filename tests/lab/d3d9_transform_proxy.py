#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Compare native PE64 Transform outputs with the supplied production PE32 proxy."""
import argparse,hashlib,json,os,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
for n in ('recovery','production-root','pair','host-service','output'):p.add_argument('--'+n,type=Path,required=True)
p.add_argument('--prefix',type=Path,help='reuse the preceding runner owned prefix after its terminal release')
a=p.parse_args();base=a.recovery.resolve();root=a.production_root.resolve();pair=a.pair.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
prefix=a.prefix.resolve() if a.prefix else out/'prefix'
source=Path(__file__).with_suffix('.c');backend=base/'backend/d3d9.dll';sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
inputs=[Path(__file__),source,pair/'receipt.json',pair/'d3d9.dll',a.host_service.resolve(),backend,*sorted((root/'wine/ps5').rglob('*.c')),*sorted((root/'wine/ps5').rglob('*.h'))]
pair_receipt=json.loads((pair/'receipt.json').read_text())
assert pair_receipt['features']==260095,'requires the complete frozen binding/draw/Transform pair'
for build in pair_receipt['builds']:
 assert '-DPW_D3D9_ENABLE_DRAW_BATCH' in build['command']
for name,digest in pair_receipt['sources'].items():
 assert sha(root/name)==digest,('pair source differs from production root',name)
r={'status':'running','scope':'Exact native Transform HRESULT/matrix comparison with production proxy, async off/on. Supplied host service must use only the documented ordinary-window test adapter. No console acceptance.','inputs':{str(f):sha(f) for f in inputs},'commands':[],'artifacts':{}}
def save():(out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n')
def run(cmd,label,env=None):
 cmd=list(map(str,cmd));entry={'command':cmd,'label':label};r['commands'].append(entry);save()
 with (out/(label+'.log')).open('w') as log:
  try:result=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,env=env,timeout=120)
  except subprocess.TimeoutExpired:entry['status']='timeout';raise
 entry['exit']=result.returncode;save();assert not result.returncode,(label,result.returncode)
 return (out/(label+'.log')).read_text(errors='replace')
def win(f):return 'Z:'+str(f.resolve()).replace('/',chr(92))
try:
 if not a.prefix:run(['cp','-a','--reflink=auto',base/'host/prefix',prefix],'prefix-copy')
 else:assert prefix.is_dir()
 for abi,cc in [('32','i686-w64-mingw32-gcc'),('64','x86_64-w64-mingw32-gcc')]:
  target=out/(abi+'.exe');run([cc,'-std=c11','-O2','-Wall','-Wextra','-Werror','-municode',source,'-o',target],'compile-'+abi);r['artifacts'][str(target)]=sha(target)
 env=os.environ.copy();env.update(WINEPREFIX=str(prefix),WINEDEBUG='-all',WINEDLLOVERRIDES='mscoree,mshtml=;d3d9=n',PW_D3D9_PROFILE='1')
 rows={};metrics={}
 for mode in ('native','0','1'):
  env['PW_D3D9_ASYNC']='0' if mode=='native' else mode
  args=[out/'64.exe',win(backend)] if mode=='native' else [out/'32.exe',win(pair/'d3d9.dll'),win(a.host_service),win(backend)]
  log=run([base/'host/build/loader/wine',*args],mode,env)
  assert 'TRANSFORM_CLOSE status=0' in log
  rows[mode]=[line for line in log.splitlines() if line.startswith(('TRANSFORM_VALUE ','TRANSFORM_RESULT '))]
  assert len(rows[mode])>=100,(mode,len(rows[mode]))
  if mode!='native':
   clients=[dict(re.findall(r'(\w+)=([^\s]+)',line)) for line in log.splitlines() if line.startswith('PW_D3D9_PROFILE transport ') and ' role=client ' in line]
   assert clients,mode
   metrics[mode]={'getter_rpcs':sum(int(x.get('op24',0)) for x in clients),'async_queued':sum(int(x.get('async_queued',0)) for x in clients),'get_calls':sum(line.startswith('TRANSFORM_VALUE ') for line in rows[mode])}
 assert rows['native']==rows['0']==rows['1'],'native/proxy HRESULT or matrix mismatch'
 assert metrics['0']['getter_rpcs']>=metrics['0']['get_calls']
 assert metrics['1']['getter_rpcs']<metrics['0']['getter_rpcs'] and metrics['1']['async_queued']>0,metrics
 assert all(sha(Path(f))==h for f,h in r['inputs'].items())
 r.update(status='pass',matched_rows=len(rows['native']),rows=rows['native'],metrics=metrics)
except BaseException as e:r.update(status='timeout' if isinstance(e,subprocess.TimeoutExpired) else 'failed',error=repr(e));raise
finally:save()
print(out/'receipt.json')
