#!/usr/bin/env python3
"""Compare generated PE32 snapshots to real Unix64 Wine parameter layouts.
No Vulkan driver or console access. Compilers are capped; no full Wine build.
"""
import argparse,hashlib,json,os,pathlib,shutil,subprocess
p=argparse.ArgumentParser();p.add_argument('--source',required=True);p.add_argument('--build',required=True);p.add_argument('--xml',required=True);p.add_argument('--video-xml',required=True);p.add_argument('--output',required=True);p.add_argument('--wine',default='wine');a=p.parse_args()
repo=pathlib.Path(__file__).resolve().parents[2];out=pathlib.Path(a.output).resolve();out.mkdir(parents=True,exist_ok=True);source=pathlib.Path(a.source).resolve();build=pathlib.Path(a.build).resolve();commands=[]
def run(argv,env=None):
 result=subprocess.run(list(map(str,argv)),capture_output=True,text=True,env=env,timeout=180);commands.append({'command':list(map(str,argv)),'exit_code':result.returncode,'stdout':result.stdout,'stderr':result.stderr});assert result.returncode==0,result.stdout+result.stderr;return result
args=['--source',source,'--xml',a.xml,'--video-xml',a.video_xml]
run(['python3',repo/'tools/generate_vk_codecs.py',*args,'--output',out]);regen=out/'regenerated';run(['python3',repo/'tools/generate_vk_codecs.py',*args,'--output',regen])
for name in ['pw_vk_generated.c','pw_vk_generated_manifest.json','pw_vk_generated_roundtrip.c']:assert (out/name).read_bytes()==(regen/name).read_bytes(),name+' not deterministic'
shutil.copy2(repo/'wine/ps5/vulkan/pw_vk_codec.h',out)
base=['-std=gnu11','-Wall','-Wextra','-Werror','-D__WINESRC__','-I'+str(source/'include'),'-I'+str(source/'dlls/winevulkan'),'-I'+str(out)]
sources=[repo/'wine/ps5/vulkan/pw_vk_codec.c',out/'pw_vk_generated.c',out/'pw_vk_generated_roundtrip.c'];cap=['systemd-run','--user','--scope','-q','-p','MemoryMax=8G']
run([*cap,'i686-w64-mingw32-gcc',*base,*sources,'-o',out/'roundtrip32.exe'])
run([*cap,'cc',*base,'-DWINE_UNIX_LIB','-D_WIN64','-I'+str(build/'include'),*sources,'-o',out/'roundtrip64'])
packets=out/'packets';packets.mkdir(exist_ok=True);env=os.environ.copy();env.update(WINEPREFIX=str(out/'wine-prefix'),WINEDEBUG='-all',WINEDLLOVERRIDES='mscoree,mshtml=')
run([a.wine,out/'roundtrip32.exe','produce','Z:'+str(packets)],env)
run([out/'roundtrip64','consume',packets])
# Compile the actual Unix batch entry with controlled typed native dispatches.
# Its header must come from this candidate, before cached Wine's old adapter.
shutil.copy2(repo/'wine/ps5/vulkan/pw_vk_batch.h',out)
shutil.copy2(source/'dlls/winevulkan/vulkan_loader.h',out)
private=(source/'dlls/winevulkan/vulkan_private.h').read_text();(out/'vulkan_private.h').write_text(private)
replay_sources=[repo/'tests/lab/vk_generated_replay.c',repo/'wine/ps5/vulkan/pw_vk_batch_unix.c',repo/'wine/ps5/vulkan/pw_vk_codec.c',out/'pw_vk_generated.c',repo/'wine/ps5/pw_vk_wire.c',repo/'wine/ps5/pw_vk_command_stream.c']
replay_flags=['-std=gnu11','-Wall','-Wextra','-Werror','-D__WINESRC__','-DWINE_UNIX_LIB','-D_WIN64','-I'+str(out),'-I'+str(repo/'wine/ps5/vulkan'),'-I'+str(repo/'wine/ps5'),'-I'+str(build/'include'),'-I'+str(source/'include'),'-I'+str(source/'dlls/winevulkan')]
run([*cap,'cc',*replay_flags,*replay_sources,'-pthread','-Wl,--wrap=pw_vk_codec_value','-Wl,--wrap=malloc','-Wl,--wrap=free','-o',out/'replay64'])
run([out/'replay64'])

manifest=json.loads((out/'pw_vk_generated_manifest.json').read_text());receipt={'schema_version':1,'status':'pass','console_accessed':False,'real_wine_layouts':True,'guest_pointer_storage_overwritten':True,'generated_opcodes':manifest['generated_thunks'],'actual_unix_replay_fixture':True,'gpu_driver_used':False,'coverage_manifest':str(out/'pw_vk_generated_manifest.json'),'commands':commands,'sha256':{str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in [repo/'tools/generate_vk_codecs.py',repo/'wine/ps5/vulkan/pw_vk_codec.c',repo/'wine/ps5/vulkan/pw_vk_codec.h',source/'dlls/winevulkan/make_vulkan',source/'dlls/winevulkan/loader_thunks.h',pathlib.Path(a.xml),pathlib.Path(a.video_xml)]}};(out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');print('PASS '+str(out/'receipt.json'))
