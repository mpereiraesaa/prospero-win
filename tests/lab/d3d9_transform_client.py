#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Compare GetTransform through the real production proxy with async off and on."""
import argparse,hashlib,json,os,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--recovery',type=Path,required=True);p.add_argument('--prefix',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
p.add_argument('--host-service',type=Path,help='reuse an existing test-only host adapter service');p.add_argument('--pair',type=Path,help='reuse an existing production pair directory (fixture-only changes)');p.add_argument('--steps',type=int,default=1500);p.add_argument('--seeds',type=int,default=3)
a=p.parse_args();base=a.recovery.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
root=Path(__file__).resolve().parents[2];lab=root/'tests/lab'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
sources=[*sorted((root/'wine/ps5').rglob('*.c')),*sorted((root/'wine/ps5').rglob('*.h')),Path(__file__),lab/'d3d9_transform_client.c',lab/'d3d9_game_build.py',lab/'d3d9_profile_transport_device.c',lab/'d3d9_transform_device.c',lab/'d3d9_surface_lifetime_device.c']
r={'status':'running','console_accessed':False,'scope':'Actual production proxy/service with draws and Transform evidence compiled, native DXVK backend; only PS5 HWND association replaced by the existing ordinary-host adapter. No console result.','sources':{str(p):sha(p) for p in sources},'commands':[]}
def save():(out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n')
def run(cmd,label,env=None,timeout=300):
 cmd=list(map(str,cmd));entry={'command':cmd};r['commands'].append(entry);save()
 with (out/(label+'.log')).open('w') as log:
  try:x=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,env=env,timeout=timeout)
  except subprocess.TimeoutExpired:entry['status']='timeout';raise
 entry['exit']=x.returncode;save();assert x.returncode==0,label
 return (out/(label+'.log')).read_text(errors='replace')
win=lambda p:'Z:'+str(p).replace('/','\\')
try:
 if a.pair:run(['cp','-a',a.pair.resolve(),out/'pair'],'pair-reuse')
 else:run(['python3',lab/'d3d9_game_build.py','--draws','--output',out/'pair'],'pair-build')
 build=json.loads((out/'pair/receipt.json').read_text())
 assert build['draws_compiled'] and not build['api_diagnostics_compiled'] and build['features']==260095 and any('pw_d3d9_transform_observer.c' in x for x in next(b for b in build['builds'] if b['file']=='d3d9.dll')['command'])
 service=next(x for x in build['builds'] if x['file']=='service.dll')['command']
 service=[str(lab/'d3d9_transform_device.c') if x.endswith('/d3d9/pw_d3d9_native_device.c') else x for x in service]
 service.insert(1,'-I'+str(root/'wine/ps5/d3d9'));service[service.index('-o')+1]=str(out/'host-service.dll')
 if a.host_service:run(['cp',a.host_service.resolve(),out/'host-service.dll'],'host-service-reuse')
 else:run(service,'host-service-build')
 run(['i686-w64-mingw32-gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-municode',lab/'d3d9_transform_client.c','-o',out/'client.exe'],'client-build')
 # Native control: the same client as PE64, calling the pinned DXVK backend directly.
 run(['x86_64-w64-mingw32-gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-municode',lab/'d3d9_transform_client.c','-o',out/'native64.exe'],'native-build')
 results=[]
 for seed in range(1,a.seeds+1):
  transcripts={}
  for mode in ('native','0','1'):
   env=os.environ.copy();env.update(WINEPREFIX=str(a.prefix.resolve()),WINEDEBUG='-all',WINEDLLOVERRIDES='mscoree,mshtml=;d3d9=n;winedbg.exe=d',
    PW_D3D9_ASYNC=mode,PW_D3D9_PROFILE='1',PW_D3D9_DIAGNOSTICS='0',DXVK_LOG_LEVEL='warn',DXVK_LOG_PATH=str(out))
   label='seed%d-async%s'%(seed,mode)
   if mode=='native':log=run([base/'host/build/loader/wine',out/'native64.exe',win(base/'backend/d3d9.dll'),'unused','unused',a.steps,seed*2654435761%2**32],label,env)
   else:log=run([base/'host/build/loader/wine',out/'client.exe',win(out/'pair/d3d9.dll'),win(out/'host-service.dll'),win(base/'backend/d3d9.dll'),a.steps,seed*2654435761%2**32],label,env)
   m=re.search(r'PW_TRANSFORM_CLIENT steps=(\d+) gets=(\d+) device_refs=0 factory_refs=0 status=0',log);assert m,label
   lines=[x for x in log.splitlines() if x.startswith(('PW_TRANSFORM_GET','PW_TRANSFORM_OP','PW_TRANSFORM_EDGE','PW_TRANSFORM_READBACK','PW_TRANSFORM_BUFFER_CHECK','PW_TRANSFORM_READBACK_PREPARE'))]
   clients=[dict(re.findall(r'(\w+)=([^\s]+)',x)) for x in log.splitlines() if x.startswith('PW_D3D9_PROFILE transport ') and ' role=client ' in x]
   transcripts[mode]=lines
   presents=[c for c in clients if c.get('final')=='0']
   controlled=presents[-2] if mode!='native' and len(presents)>=2 else {}
   reads=[x for x in lines if x.startswith('PW_TRANSFORM_READBACK ')]
   final=[x for x in reads if 'label=end' in x or 'label=start' in x]
   assert len(final)==4 and all(' hr=00000000 center=ffff0000 ' in x for x in final),final  # UP and DP/DIP buffer draws
   results.append(dict(seed=seed,async_mode=mode,gets=int(m.group(2)),getter_rpcs=sum(int(c.get('op24',0)) for c in clients),
    command_rpcs=sum(int(c.get('op21',0)) for c in clients),batch_flushes=sum(int(c.get('op33',0)) for c in clients),
    async_queued=sum(int(c.get('async_queued',0)) for c in clients),readbacks=len([x for x in lines if x.startswith('PW_TRANSFORM_READBACK ')]),controlled_interval={k:controlled.get(k,'0') for k in ('frame','op21','op24','op33','async_queued','batch_commands','sync_published')},total_published=sum(int(c.get('sync_published',0)) for c in clients),transcript_lines=len(lines),
    transcript_sha256=hashlib.sha256('\n'.join(lines).encode()).hexdigest()))
  assert transcripts['native']==transcripts['0'],'seed %d: bridge differs from direct DXVK'%seed
  assert transcripts['0']==transcripts['1'],'seed %d: async on differs from DXVK answers'%seed
  off,on=results[-2],results[-1]
  co,cn=off['controlled_interval'],on['controlled_interval']
  # Valid-state buffer DP/DIP readback (asserted red above) went through batches with async on.
  assert co['frame']==cn['frame'] and int(cn['batch_commands'])>0 and int(cn['async_queued'])>0 and int(co['async_queued'])==0 and int(cn['op21'])<int(co['op21']),(co,cn)
  assert off['getter_rpcs']>=off['gets'] and on['getter_rpcs']<off['getter_rpcs'] and on['async_queued']>0,(off,on)
 assert all(sha(Path(p))==h for p,h in r['sources'].items()),'source changed during proof'
 r['artifacts']={str(p):sha(p) for p in [out/'pair/d3d9.dll',out/'host-service.dll',out/'client.exe',out/'native64.exe',base/'backend/d3d9.dll']}
 r.update(status='pass',results=results)
except BaseException as e:r.update(status='timeout' if isinstance(e,subprocess.TimeoutExpired) else 'failed',error=repr(e));raise
finally:save()
print('PASS '+str(out/'receipt.json'))
