#!/usr/bin/env python3
"""Actual production session transport with a test-only ordinary native window."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
for n in ('wine-build','prefix','backend64','output'):p.add_argument('--'+n,type=Path,required=True)
a=p.parse_args();a.prefix.resolve().mkdir(parents=True,exist_ok=True);root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
common=[root/'wine/ps5'/n for n in ['d3d9/pw_d3d9_session.c','pw_d3d9_bridge_wire.c','pw_d3d9_objects.c','pw_d3d9_factory_wire.c','pw_d3d9_device_wire.c','pw_d3d9_resource_wire.c','pw_d3d9_command_wire.c','pw_d3d9_getter_wire.c','pw_d3d9_cursor_wire.c','pw_d3d9_texture_wire.c']]
native=[root/'wine/ps5/d3d9'/n for n in ['pw_d3d9_service_resource.c','pw_d3d9_native_resource.c','pw_d3d9_service_methods.c','pw_d3d9_native_command.c','pw_d3d9_native_getter.c','pw_d3d9_cursor.c','pw_d3d9_service_texture.c','pw_d3d9_native_texture.c']]
fixture=Path(__file__).with_suffix('.c');adapter=fixture.with_name('d3d9_cursor_transport_device.c');sources=[*common,*native,fixture,adapter,root/'wine/ps5/d3d9/pw_d3d9_cursor.c',Path(__file__)];sources+=sorted((root/'wine/ps5').rglob('*.h'));frozen={str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in sources};r={'status':'running','console_accessed':False,'test_only_window_adapter':True,'sources':frozen,'commands':[]}
def run(cmd,label,env=None,expected=0):
 command=list(map(str,cmd))
 try:
  x=subprocess.run(command,text=True,capture_output=True,env=env,timeout=90)
 except subprocess.TimeoutExpired as error:
  stdout=error.stdout or '';stderr=error.stderr or ''
  if isinstance(stdout,bytes):stdout=stdout.decode('utf-8',errors='replace')
  if isinstance(stderr,bytes):stderr=stderr.decode('utf-8',errors='replace')
  (out/(label+'.log')).write_text(stdout+stderr)
  r['status']='timeout';r['commands'].append(dict(command=command,exit_code=None,timeout_seconds=90,stdout=stdout,stderr=stderr))
  (out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n')
  raise
 (out/(label+'.log')).write_text(x.stdout+x.stderr)
 r['commands'].append(dict(command=command,exit_code=x.returncode,stdout=x.stdout,stderr=x.stderr))
 if x.returncode!=expected:r['status']='failed'
 (out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n')
 assert x.returncode==expected,(label,x.returncode,x.stdout+x.stderr)
 return x
flags=['-std=gnu11','-O2','-Wall','-Wextra','-Werror','-Wno-array-bounds',*['-DPW_D3D9_ENABLE_'+x for x in ['DEVICE','RESOURCE','METHODS','CURSOR','TEXTURE']]]
run(['i686-w64-mingw32-gcc',*flags,'-municode',fixture,*common,root/'wine/ps5/d3d9/pw_d3d9_cursor.c','-o',out/'client.exe'],'client-build')
run(['x86_64-w64-mingw32-gcc',*flags,'-shared','-static-libgcc',*common,*native,adapter,'-luuid','-ldxguid','-luser32','-o',out/'service.dll'],'service-build')
def win(p):return 'Z:'+str(p.resolve()).replace('/',chr(92))
env=os.environ.copy();env.update(WINEPREFIX=str(a.prefix.resolve()),WINEDEBUG='-all',WINEDLLOVERRIDES='mscoree,mshtml=;d3d9=n',DXVK_LOG_PATH=str(out))
x=run([a.wine_build.resolve()/'loader/wine',out/'client.exe',win(out/'service.dll'),win(a.backend64)],'transport',env)
assert x.stdout.count('status=0 exact_bool=1 negatives=4')==3
assert frozen=={str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in sources}
r.update(status='pass',sources=frozen,artifacts={str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in [out/'client.exe',out/'service.dll',a.backend64]})
(out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n');print(r['status']+' '+str(out/'receipt.json'))
