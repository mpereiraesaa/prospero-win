#!/usr/bin/env python3
"""Compile and execute the real PE runtime, mock only the Unix boundary.
Optional XML arguments additionally prove generator byte-for-byte consistency.
"""
import argparse,contextlib,hashlib,io,json,os,pathlib,runpy,shutil,subprocess
p=argparse.ArgumentParser();p.add_argument('--wine-source',required=True);p.add_argument('--wine-build',required=True);p.add_argument('--output',required=True);p.add_argument('--vk-xml');p.add_argument('--video-xml');a=p.parse_args()
r=pathlib.Path(__file__).resolve().parents[1];out=pathlib.Path(a.output).resolve();out.mkdir(parents=True,exist_ok=True);source=out/'source';d=source/'dlls/winevulkan';d.parent.mkdir(parents=True,exist_ok=True)
assert not d.exists(),'output source must be fresh';shutil.copytree(pathlib.Path(a.wine_source)/'dlls/winevulkan',d)
results=[]
def run(command,env=None):
 x=subprocess.run(command,capture_output=True,text=True,env=env,timeout=90);results.append(dict(command=list(map(str,command)),exit_code=x.returncode,stdout=x.stdout,stderr=x.stderr));assert x.returncode==0,x.stdout+x.stderr;return x
run(['python3',str(r/'tools/stage_vk_batch.py'),'--source',str(source)])
assert 'pw_vk_batch_thread_detach();' in (d/'loader.c').read_text()
assert 'DisableThreadLibraryCalls(hinst);' not in (d/'loader.c').read_text()
assert (d/'vulkan_thunks.c').read_text().count('    pw_vk_batch_unix,')==2
if a.vk_xml and a.video_xml:
 old=os.getcwd();os.chdir(d)
 try:
  with contextlib.redirect_stdout(io.StringIO()),contextlib.redirect_stderr(io.StringIO()):
   m=runpy.run_path(str(d/'make_vulkan'),run_name='runtime_test');g=m['Generator'](a.vk_xml,a.video_xml)
   for name,method in [('loader_thunks.h','generate_loader_thunks_h'),('loader_thunks.c','generate_loader_thunks_c'),('vulkan_thunks.c','generate_thunks_c')]:
    f=io.StringIO();getattr(g,method)(f);assert f.getvalue()==(d/name).read_text(),name+' regeneration differs'
 finally:os.chdir(old)
includes=['-I'+str(pathlib.Path(a.wine_source)/'include'),'-I'+str(d)]
exe=out/'test_pe.exe'
run(['i686-w64-mingw32-gcc','-std=gnu11','-Wall','-Wextra','-Werror','-D__WINESRC__',*includes,str(r/'tests/test_vk_batch_pe.c'),*[str(d/(name+'.c')) for name in ['pw_vk_command_stream','pw_vk_wire','pw_vk_template_cache']],'-o',str(exe)])
run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-D__WINESRC__','-DWINE_UNIX_LIB','-D_WIN64','-I'+str(pathlib.Path(a.wine_build)/'include'),*includes,'-c',str(d/'pw_vk_batch_unix.c'),'-o',str(out/'unix.o')])
env=os.environ.copy();env.update(WINEPREFIX=str(out/'wine-prefix'),WINEDEBUG='-all',WINEDLLOVERRIDES='mscoree,mshtml=')
for mode in ['on','old','off','stats']:run([str(pathlib.Path(a.wine_build)/'loader/wine'),str(exe),mode],env)
receipt={'schema_version':1,'status':'pass','actual_pe_runtime_win32_apis':True,'unix_boundary_mocked':True,'automatic_dll_notification_runtime_tested':False,'automatic_notification_source_wiring_checked':True,'generator_regenerated_equal':bool(a.vk_xml and a.video_xml),'console_accessed':False,'commands':results,'hashes':{str(x.relative_to(r)):hashlib.sha256(x.read_bytes()).hexdigest() for x in [r/'wine/ps5/vulkan/pw_vk_batch_pe.c',r/'wine/ps5/vulkan/pw_vk_batch_unix.c',r/'tools/stage_vk_batch.py',r/'tests/test_vk_batch_pe.c']}}
(out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');print('PASS actual PE runtime modes on/old/off/stats, thread retirement, callback reentry, global ordering; '+str(out/'receipt.json'))
