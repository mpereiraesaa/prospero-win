#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Exercise typed shader/declaration proxies with controlled transport callbacks."""
import argparse, hashlib, json, os, subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
for name in ('wine-build','prefix','output'): p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args(); root=Path(__file__).resolve().parents[2]; out=a.output.resolve(); out.mkdir(parents=True,exist_ok=False)
files=['tests/lab/d3d9_program_proxy.c','wine/ps5/d3d9/pw_d3d9_program_proxy.c','wine/ps5/d3d9/pw_d3d9_program_proxy.h','wine/ps5/pw_d3d9_program_wire.c','wine/ps5/pw_d3d9_program_wire.h','wine/ps5/d3d9/pw_d3d9_session.h']
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
r={'sources':{f:sha(root/f) for f in files},'tests':[],'scope':'Controlled COM ABI/ownership proof, no backend substitution in production.'}
def run(command,name,env=None):
 x=subprocess.run(list(map(str,command)),env=env,capture_output=True,text=True,timeout=120); (out/(name+'.log')).write_text(x.stdout+x.stderr); r['tests'].append({'name':name,'exit':x.returncode}); assert not x.returncode,name; return x.stdout
try:
 for arch in ('i686','x86_64'):
  exe=out/(arch+'.exe'); run([arch+'-w64-mingw32-gcc','-std=c11','-O2','-Wall','-Wextra','-Werror',root/files[0],root/files[1],root/files[3],'-luuid','-ldxguid','-o',exe],arch+'-build');r[arch]=sha(exe)
  env=os.environ.copy();env.update(WINEPREFIX=str(a.prefix.resolve()),WINEDEBUG='-all')
  text=run([a.wine_build.resolve()/'loader/wine',exe],arch+'-run',env)
  assert 'PW_PROGRAM_PROXY shader=1 declaration=1 identity=1 deferred=1 safe_read=1 status=0' in text
 r['status']='pass'
finally: (out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n')
print(json.dumps(r,indent=2))
