#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Correlate actual PE32 proxy/PE64 service profiling through real D3DX smoke."""
import argparse,hashlib,json,os,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--recovery',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
a=p.parse_args();base=a.recovery.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
root=Path(__file__).resolve().parents[2];lab=root/'tests/lab'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
sources=[*sorted((root/'wine/ps5').rglob('*.c')),*sorted((root/'wine/ps5').rglob('*.h')),Path(__file__),*[lab/n for n in ['d3d9_game_build.py','d3d9_game_smoke.py','d3d9_game_smoke.c','d3d9_game_smoke_fx.h','d3d9_surface_lifetime_device.c','d3d9_profile_transport_device.c']]]
r={'status':'running','scope':'Actual production proxy/service and native DXVK; only PS5 HWND association replaced by existing ordinary-host adapter. No console result.','sources':{str(p):sha(p) for p in sources},'commands':[]}
def save():(out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n')
def run(cmd,label,env=None):
 cmd=list(map(str,cmd));entry={'command':cmd};r['commands'].append(entry);save()
 with (out/(label+'.log')).open('w') as log:
  try:x=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,env=env,timeout=180)
  except subprocess.TimeoutExpired:entry['status']='timeout';raise
 entry['exit']=x.returncode;save();assert x.returncode==0,label
try:
 run(['python3',lab/'d3d9_game_build.py','--api-diagnostics','--output',out/'pair'],'pair-build')
 build=json.loads((out/'pair/receipt.json').read_text())
 service=next(x for x in build['builds'] if x['file']=='service.dll')['command']
 adapter=lab/'d3d9_profile_transport_device.c'
 service=[str(adapter) if x.endswith('/d3d9/pw_d3d9_native_device.c') else x for x in service]
 service.insert(1,'-I'+str(root/'wine/ps5/d3d9'))
 service[service.index('-o')+1]=str(out/'host-service.dll')
 run(service,'host-service-build')
 run(['cp','-a','--reflink=auto',base/'host/prefix',out/'prefix'],'prefix-copy')
 env=os.environ.copy();env.update(PW_D3D9_PROFILE='1',PW_D3D9_DIAGNOSTICS='0',WINEDEBUG='-all')
 d3dx=base/'host/build/dlls/d3dx9_43/i386-windows/d3dx9_43.dll'
 run(['python3',lab/'d3d9_game_smoke.py','--wine-build',base/'host/build','--prefix',out/'prefix','--proxy',out/'pair/d3d9.dll','--service',out/'host-service.dll','--backend64',base/'backend/d3d9.dll','--d3dx',d3dx,'--output',out/'smoke'],'smoke',env)
 log=(out/'smoke/smoke.log').read_text(errors='replace')
 def records(prefix):
  return [dict(re.findall(r'(\w+)=([^\s]+)',line)) for line in log.splitlines() if line.startswith(prefix)]
 transports=records('PW_D3D9_PROFILE transport ');api=records('PW_D3D9_API_PROFILE ')
 clients=[x for x in transports if x['role']=='client' and x['final']=='0']
 services=[x for x in transports if x['role']=='service' and x['final']=='0']
 boundaries=[x for x in api if x['boundary']=='present']
 assert len(clients)==len(services)==len(boundaries)==3,(clients,services,boundaries)
 for c in clients:
  key=(c['epoch'],c['seq'],c['object'],c['generation'])
  matching=[s for s in services if (s['epoch'],s['seq'],s['object'],s['generation'])==key];assert len(matching)==1
  matching_api=[s for s in boundaries if (s['epoch'],s['sequence'],s['object'],s['generation'])==key];assert len(matching_api)==1
  s=matching[0];b=matching_api[0]
  assert c['clock_valid']==s['clock_valid']=='1' and c['hr']==s['hr']=='00000000'
  assert c['sync_published']==s['sync_published']==c['replies']==s['replies']
  assert c['request_bytes']==s['request_bytes'] and c['reply_bytes']==s['reply_bytes']
  assert c['async_queued']==s['async_queued']=='0'
  assert int(c['sync_published'])>1 and int(b['api_external_vtable_entries'])>0 and b['classification_valid']=='1'
 assert len([x for x in api if x['boundary']=='session_close'])==3
 assert all(sha(Path(p))==h for p,h in r['sources'].items())
 r['artifacts']={str(p):sha(p) for p in [out/'pair/d3d9.dll',out/'host-service.dll',out/'smoke/client.exe',base/'backend/d3d9.dll',d3dx]}
 r.update(status='pass',matched_present_boundaries=len(clients),transport_records=transports,api_records=api)
except BaseException as e:r.update(status='timeout' if isinstance(e,subprocess.TimeoutExpired) else 'failed',error=repr(e));raise
finally:save()
print(out/'receipt.json')
