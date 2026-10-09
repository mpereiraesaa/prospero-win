#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Build paired production PE32 proxy and native PE64 D3D9 service."""
import subprocess,pathlib,json,hashlib,argparse
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=pathlib.Path,required=True);args=parser.parse_args()
root=pathlib.Path(__file__).resolve().parents[2]
out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
flags=['-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-array-bounds','-DPW_D3D9_ENABLE_DEVICE','-DPW_D3D9_ENABLE_RESOURCE','-DPW_D3D9_ENABLE_METHODS','-DPW_D3D9_ENABLE_PROGRAM','-DPW_D3D9_ENABLE_TEXTURE','-DPW_D3D9_ENABLE_STATEBLOCK','-DPW_D3D9_ENABLE_OBJECT_GETTER','-DPW_D3D9_ENABLE_UP']
shared=['d3d9/pw_d3d9_session.c','pw_d3d9_bridge_wire.c','pw_d3d9_objects.c','pw_d3d9_factory_wire.c','pw_d3d9_device_wire.c','pw_d3d9_resource_wire.c','pw_d3d9_command_wire.c','pw_d3d9_getter_wire.c','pw_d3d9_program_wire.c','pw_d3d9_program_query.c','pw_d3d9_texture_wire.c','pw_d3d9_stateblock_wire.c','pw_d3d9_object_getter.c','pw_d3d9_up_wire.c']
client=['d3d9/pw_d3d9_proxy.c','d3d9/pw_d3d9_device_proxy.c','d3d9/pw_d3d9_device_methods.c','d3d9/pw_d3d9_program_proxy.c','d3d9/pw_d3d9_buffer_proxy.c','d3d9/pw_d3d9_buffer_client.c','d3d9/pw_d3d9_staging.c','d3d9/pw_d3d9_texture_client.c','d3d9/pw_d3d9_texture_proxy.c','d3d9/pw_d3d9_private_data.c','d3d9/pw_d3d9_stateblock_client.c','d3d9/pw_d3d9_device_object_methods.c','d3d9/pw_d3d9_up_client.c']
service=['d3d9/pw_d3d9_native_device.c','d3d9/pw_d3d9_native_resource.c','d3d9/pw_d3d9_native_command.c','d3d9/pw_d3d9_native_getter.c','d3d9/pw_d3d9_native_program.c','d3d9/pw_d3d9_native_program_query.c','d3d9/pw_d3d9_service_resource.c','d3d9/pw_d3d9_service_methods.c','d3d9/pw_d3d9_service_program.c','d3d9/pw_d3d9_service_texture.c','d3d9/pw_d3d9_native_texture.c','d3d9/pw_d3d9_service_stateblock.c','d3d9/pw_d3d9_native_stateblock.c','d3d9/pw_d3d9_service_object_getter.c','d3d9/pw_d3d9_native_object_getter.c','d3d9/pw_d3d9_service_up.c','d3d9/pw_d3d9_native_up.c']
r={'features':255,'sources':{},'builds':[],'scope':'Paired production build; runtime acceptance is separate.'}
for path in sorted((root/'wine/ps5').rglob('*.h')):r['sources'][str(path.relative_to(root))]=hashlib.sha256(path.read_bytes()).hexdigest()
for arch,extra,output in [('i686',client,'d3d9.dll'),('x86_64',service,'service.dll')]:
 paths=[root/'wine/ps5'/n for n in shared+extra]
 for path in paths:r['sources'][str(path.relative_to(root))]=hashlib.sha256(path.read_bytes()).hexdigest()
 cmd=[arch+'-w64-mingw32-gcc',*flags,'-shared','-static-libgcc','-Wl,--kill-at',*map(str,paths),'-luuid','-ldxguid','-o',str(out/output)]
 result=subprocess.run(cmd,capture_output=True,text=True);(out/(output+'.log')).write_text(result.stdout+result.stderr);r['builds'].append({'file':output,'exit':result.returncode});assert not result.returncode,result.stderr
 r[output]=hashlib.sha256((out/output).read_bytes()).hexdigest()
(out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r['builds']))
