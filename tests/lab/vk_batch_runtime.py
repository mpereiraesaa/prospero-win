#!/usr/bin/env python3
"""Compile and execute the real PE runtime, mock only the Unix boundary.
Optional XML arguments additionally prove generator byte-for-byte consistency.
"""
import argparse,contextlib,hashlib,io,json,os,pathlib,re,runpy,shutil,subprocess
p=argparse.ArgumentParser();p.add_argument('--wine-source',required=True);p.add_argument('--wine-build',required=True);p.add_argument('--output',required=True);p.add_argument('--vk-xml');p.add_argument('--video-xml');a=p.parse_args()
r=pathlib.Path(__file__).resolve().parents[2];out=pathlib.Path(a.output).resolve();out.mkdir(parents=True,exist_ok=True);source=out/'source';d=source/'dlls/winevulkan';d.parent.mkdir(parents=True,exist_ok=True)
assert not d.exists(),'output source must be fresh';shutil.copytree(pathlib.Path(a.wine_source)/'dlls/winevulkan',d)
results=[]
def run(command,env=None,expected=0):
 if command[0] in ['cc','i686-w64-mingw32-gcc']:command=['systemd-run','--user','--scope','-q','-p','MemoryMax=8G',*command]
 x=subprocess.run(command,capture_output=True,text=True,env=env,timeout=90);results.append(dict(command=list(map(str,command)),exit_code=x.returncode,stdout=x.stdout,stderr=x.stderr));assert x.returncode==expected,x.stdout+x.stderr;return x
run(['python3',str(r/'tools/stage_vk_batch.py'),'--source',str(source)])
assert 'pw_vk_batch_thread_detach();' in (d/'loader.c').read_text()
assert '#ifdef _WIN64\n            DisableThreadLibraryCalls(hinst);\n#else' in (d/'loader.c').read_text()
assert (d/'vulkan_thunks.c').read_text().count('    pw_vk_batch_unix,')==2
enum_names=re.findall(r'^    unix_(\w+),$',(d/'loader_thunks.h').read_text().split('enum unix_call',1)[1].split('};',1)[0],re.M)[:-1]
name_rows=re.findall(r' \[unix_(\w+)\] = "([^"]+)",',(d/'pw_vk_function_names.h').read_text())
assert name_rows==[(name,name) for name in enum_names]
if a.vk_xml and a.video_xml:
 old=os.getcwd();os.chdir(d)
 try:
  with contextlib.redirect_stdout(io.StringIO()),contextlib.redirect_stderr(io.StringIO()):
   m=runpy.run_path(str(d/'make_vulkan'),run_name='runtime_test');g=m['Generator'](a.vk_xml,a.video_xml)
   for name,method in [('loader_thunks.h','generate_loader_thunks_h'),('loader_thunks.c','generate_loader_thunks_c'),('vulkan_thunks.c','generate_thunks_c')]:
    f=io.StringIO();getattr(g,method)(f);assert f.getvalue()==(d/name).read_text(),name+' regeneration differs'
 finally:os.chdir(old)
includes=['-I'+str(pathlib.Path(a.wine_source)/'include'),'-I'+str(d)]
generated=out/'generated.o'
run(['i686-w64-mingw32-gcc','-std=gnu11','-Wall','-Wextra','-Werror','-D__WINESRC__',*includes,'-c',str(d/'pw_vk_generated.c'),'-o',str(generated)])
exe=out/'test_pe.exe'
run(['i686-w64-mingw32-gcc','-std=gnu11','-Wall','-Wextra','-Werror','-D__WINESRC__',*includes,str(r/'tests/lab/vk_batch_pe.c'),*[str(d/(name+'.c')) for name in ['pw_vk_command_stream','pw_vk_spsc','pw_vk_wire','pw_vk_template_cache','pw_vk_retire','pw_vk_codec']],str(generated),'-o',str(exe)])
defer=out/'test_defer.exe'
run(['i686-w64-mingw32-gcc','-std=gnu11','-Wall','-Wextra','-Werror','-D__WINESRC__',*includes,str(r/'tests/lab/vk_batch_defer.c'),*[str(d/(name+'.c')) for name in ['pw_vk_command_stream','pw_vk_spsc','pw_vk_wire','pw_vk_template_cache','pw_vk_retire','pw_vk_codec']],str(generated),'-o',str(defer)])
progress=out/'test_progress.exe'
run(['i686-w64-mingw32-gcc','-std=gnu11','-Wall','-Wextra','-Werror','-D__WINESRC__',*includes,str(r/'tests/lab/vk_batch_progress.c'),*[str(d/(name+'.c')) for name in ['pw_vk_command_stream','pw_vk_spsc','pw_vk_wire','pw_vk_template_cache','pw_vk_retire','pw_vk_codec']],str(generated),'-o',str(progress)])
owned=out/'test_owned.exe'
run(['i686-w64-mingw32-gcc','-std=gnu11','-Wall','-Wextra','-Werror','-D__WINESRC__',*includes,str(r/'tests/lab/vk_batch_owned.c'),*[str(d/(name+'.c')) for name in ['pw_vk_command_stream','pw_vk_spsc','pw_vk_wire','pw_vk_template_cache','pw_vk_retire','pw_vk_codec']],str(generated),'-o',str(owned)])
run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-D__WINESRC__','-DWINE_UNIX_LIB','-D_WIN64','-I'+str(pathlib.Path(a.wine_build)/'include'),*includes,'-c',str(d/'pw_vk_batch_unix.c'),'-o',str(out/'unix.o')])
env=os.environ.copy();env.update(WINEPREFIX=str(out/'wine-prefix'),WINEDEBUG='-all',WINEDLLOVERRIDES='mscoree,mshtml=')
env.pop('PW_VK_BATCH_MASK',None)
for mode in ['core','khr']:run([str(pathlib.Path(a.wine_build)/'loader/wine'),str(owned),mode],env)
for mode in ['on','old','off','stats','profile-no-stats','profile','append-during-replay','stall-report','disable-inflight','async-lifetime','async-disable']:
 result=run([str(pathlib.Path(a.wine_build)/'loader/wine'),str(exe),mode],env)
 if mode=='profile':
  lines=[line for line in result.stderr.splitlines() if line.startswith('PW_VK_FALLBACK')]
  assert len(lines)==16,lines
  assert 'start_present=0 end_present=300 calls=2401' in lines[0]
  assert 'rank=1' in lines[1] and 'function=vkCmdPipelineBarrier2 calls=1200 cumulative=1200' in lines[1]
  assert 'rank=2' in lines[2] and 'function=vkCmdSetViewport calls=900 cumulative=900' in lines[2]
  assert 'start_present=300 end_present=600 calls=300' in lines[5]
  assert 'function=vkQueuePresentKHR calls=300 cumulative=600' in lines[6]
  assert 'calls=1000' in lines[7] and 'top=8' in lines[7]
  for code,line in enumerate(lines[8:]):assert f'rank={code+1} code={code} ' in line and 'calls=100 ' in line
 else:assert 'PW_VK_FALLBACK' not in result.stderr
for mode in ['offstats','stats']:run([str(pathlib.Path(a.wine_build)/'loader/wine'),str(progress),mode],env)
for mode in ['off','on']:
 result=run([str(pathlib.Path(a.wine_build)/'loader/wine'),str(defer),mode],env)
 assert ('3 deferred' if mode=='on' else '0 deferred') in result.stdout,result.stdout
run([str(pathlib.Path(a.wine_build)/'loader/wine'),str(progress),'replay-failure'],env,expected=3)
maskexe=out/'test_masks.exe'
run(['i686-w64-mingw32-gcc','-std=gnu11','-Wall','-Wextra','-Werror','-D__WINESRC__',*includes,str(r/'tests/lab/vk_batch_masks.c'),*[str(d/(name+'.c')) for name in ['pw_vk_command_stream','pw_vk_spsc','pw_vk_wire','pw_vk_template_cache','pw_vk_retire','pw_vk_codec']],str(generated),'-o',str(maskexe)])
mask_cases=[('unset','127'),('0','0'),('127','127'),('0x7f','127'),('32','32'),('95','95'),*[(str(1<<i),str(1<<i)) for i in range(7)],*[(bad,'255') for bad in ['128','-1','0x','0x80','1x',' 1','999999999999999999999999999999999999']]]
for value,expected in mask_cases:run([str(pathlib.Path(a.wine_build)/'loader/wine'),str(maskexe),value,expected],env)
receipt={'schema_version':1,'opcode_mask_cases':len(mask_cases),'status':'pass','actual_pe_runtime_win32_apis':True,'template_producer_forms':6,'template_metadata_create_aliases':2,'deferred_client_lifetime_tested':True,'deferred_descriptor_writes_tested':True,'unix_boundary_mocked':True,'automatic_dll_notification_runtime_tested':False,'automatic_notification_source_wiring_checked':True,'generator_regenerated_equal':bool(a.vk_xml and a.video_xml),'console_accessed':False,'commands':results,'hashes':{str(x.relative_to(r)):hashlib.sha256(x.read_bytes()).hexdigest() for x in [r/'wine/ps5/pw_vk_spsc.c',r/'wine/ps5/pw_vk_spsc.h',r/'wine/ps5/vulkan/pw_vk_batch_pe.c',r/'wine/ps5/vulkan/pw_vk_batch_unix.c',r/'tools/stage_vk_batch.py',r/'tests/lab/vk_batch_pe.c',r/'tests/lab/vk_batch_owned.c',r/'tests/lab/vk_batch_runtime.py',r/'docs/vulkan-fallback-profile.md',r/'wine/ps5/vulkan/pw_vk_progress_guard.h',r/'tests/lab/vk_batch_progress.c',r/'docs/vulkan-progress-gate.md',r/'tests/lab/vk_batch_masks.c',r/'wine/ps5/vulkan-wire.md']}}
(out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');print('PASS actual PE runtime original paths, profiling counts/deltas/top8/ties, stats-off suppression, thread retirement, callback reentry, global ordering; '+str(out/'receipt.json'))
