#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Production PE32 pipeline session with a controlled concurrent wire peer."""
import argparse, hashlib, json, os, subprocess
from pathlib import Path
p=argparse.ArgumentParser()
for name in ('wine-build','prefix','output','source-root'):
    p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();root=a.source_root.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
fixture=Path(__file__).with_suffix('.c').resolve()
names=['pw_d3d9_bridge_wire.c','pw_d3d9_objects.c','pw_d3d9_factory_wire.c',
       'pw_d3d9_command_wire.c','pw_d3d9_getter_wire.c','pw_d3d9_command_batch.c',
       'pw_d3d9_command_policy.c','pw_d3d9_binding_plan.c','pw_d3d9_batch_pipeline.c',
       'pw_d3d9_device_wire.c','pw_d3d9_stateblock_wire.c','pw_d3d9_object_getter.c']
companion=root/'tests/lab/d3d9_pipeline_lifetime.c'
sources=[Path(__file__).resolve(),fixture,companion,root/'tests/lab/d3d9_binding_client.c',root/'wine/ps5/d3d9/pw_d3d9_session.c',
         *[root/'wine/ps5'/n for n in names],*sorted((root/'wine/ps5').rglob('*.h'))]
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
r={'status':'running','scope':'Actual PE32 production session, ledger, codecs and private ticket callbacks; controlled concurrent wire peer. No native COM/GPU or console acceptance.',
   'sources':{str(f):sha(f) for f in sources},'commands':[],'artifacts':{}}
env=os.environ.copy();env.update(WINEPREFIX=str(a.prefix.resolve()),WINEDEBUG='-all',WINEDLLOVERRIDES='mscoree,mshtml=')
def save(): (out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n')
def run(command,label):
    e={'command':command,'log':label+'.log','status':'running'};r['commands'].append(e);save()
    try:
        with (out/e['log']).open('w') as log:
            child=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,env=env,timeout=90)
        e.update(status='pass' if child.returncode==0 else 'failed',exit=child.returncode);save()
        assert child.returncode==0,(label,child.returncode)
    except subprocess.TimeoutExpired:
        e['status']='timeout';r['status']='timeout';save();raise
    return (out/e['log']).read_text()
try:
    for label,source,define,marker in [
        ('client',fixture,'-DPIPELINE_SESSION_SOURCE="'+str(root/'wine/ps5/d3d9/pw_d3d9_session.c')+'"','PIPELINE_CLIENT PASS'),
        ('lifetime',companion,'-DPIPELINE_BINDING_SOURCE="'+str(root/'tests/lab/d3d9_binding_client.c')+'"','PIPELINE_LIFETIME PASS')]:
        binary=out/(label+'.exe')
        command=['i686-w64-mingw32-gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-array-bounds',
                 '-I'+str(root/'wine/ps5'),define,str(source),*[str(root/'wine/ps5'/n) for n in names],'-luuid','-ldxguid','-o',str(binary)]
        run(command,label+'-compile');r['artifacts'][str(binary)]=sha(binary);save()
        assert marker in run([str(a.wine_build.resolve()/'loader/wine'),str(binary)],label+'-run')
    assert all(sha(Path(f))==h for f,h in r['sources'].items());r['status']='pass'
except BaseException as error:
    if r['status']!='timeout':r['status']='failed'
    r['error']=repr(error);raise
finally:save()
