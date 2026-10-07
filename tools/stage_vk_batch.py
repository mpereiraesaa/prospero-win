#!/usr/bin/env python3
"""Stage reviewed batch runtime and update pinned Wine/generator consistently.
Use an isolated source tree. Generated changes mirror generator changes.
"""
import argparse,pathlib,re,shutil,ast
ap=argparse.ArgumentParser();ap.add_argument('--source',required=True);ap.add_argument('--repo',default=str(pathlib.Path(__file__).resolve().parents[1]));a=ap.parse_args();repo=pathlib.Path(a.repo);d=pathlib.Path(a.source)/'dlls/winevulkan'
def edit(name,fn):
 p=d/name;s=p.read_text();p.write_text(fn(s))
def once(s,old,new):
 assert s.count(old)==1,(old,s.count(old));return s.replace(old,new)
for name in ['pw_vk_wire','pw_vk_template_cache','pw_vk_command_stream']:
 for ext in ['c','h']:shutil.copyfile(repo/'wine/ps5'/f'{name}.{ext}',d/f'{name}.{ext}')
for p in (repo/'wine/ps5/vulkan').glob('*.[ch]'):shutil.copyfile(p,d/p.name)
edit('vulkan_loader.h',lambda s:once(s,'#define UNIX_CALL(code, params) WINE_UNIX_CALL(unix_ ## code, params)', '#include "pw_vk_batch.h"\n#ifdef _WIN64\n#define UNIX_CALL(code, params) WINE_UNIX_CALL(unix_ ## code, params)\n#else\n#define UNIX_CALL(code, params) pw_vk_batch_call(unix_ ## code, params)\n#endif'))
edit('loader.c',lambda s:once(s,'            DisableThreadLibraryCalls(hinst);','            /* Stream nodes survive teardown; notifications retire only. */').replace('        case DLL_PROCESS_ATTACH:', '        case DLL_THREAD_DETACH:\n#ifndef _WIN64\n            pw_vk_batch_thread_detach();\n#endif\n            break;\n        case DLL_PROCESS_ATTACH:'))
edit('vulkan.c',lambda s:once(s,'    struct vulkan_instance *instance = vulkan_instance_from_handle(handle);\n    return !!vk_funcs->p_vkGetInstanceProcAddr(instance->host.instance, name);', '    struct vulkan_instance *instance = vulkan_instance_from_handle(handle);\n    if (!strcmp(name, PW_VK_BATCH_NAME)) return PW_VK_BATCH_CAPABILITY;\n    return !!vk_funcs->p_vkGetInstanceProcAddr(instance->host.instance, name);'))
names=['pw_vk_wire','pw_vk_template_cache','pw_vk_command_stream']
sources=['pw_vk_batch_pe.c','pw_vk_batch_unix.c']
for name in names:
 sources.extend([name+'.c',name+'_unix.c'])
 (d/(name+'_unix.c')).write_text('#if 0\n#pragma makedep unix\n#endif\n#include "'+name+'.c"\n')
replacement=''.join('\t'+name+' '+chr(92)+'\n' for name in sources)
edit('Makefile.in',lambda s:once(s,'\tloader.c '+chr(92),replacement+'\tloader.c '+chr(92)))
edit('loader_thunks.h',lambda s:once(s,'    unix_count,','    unix_pw_vk_batch,\n    unix_count,'))
# Every custom allocator can run guest callbacks, not just instance creation.
allocator_params=re.findall(r'struct (\w+)_params\n\{([^}]+)\};',(d/'loader_thunks.h').read_text())
allocator_names=[name for name,body in allocator_params if 'VkAllocationCallbacks *pAllocator' in body]
with (d/'pw_vk_batch_pe.c').open('a') as f:
 f.write('\n#ifndef _WIN64\nBOOL pw_vk_batch_allocator(unsigned int code,const void *args)\n{\n switch(code){\n')
 for name in allocator_names:f.write(f' case unix_{name}: return ((const struct {name}_params *)args)->pAllocator!=NULL;\n')
 f.write(' default:return FALSE;\n }\n}\n#endif\n')

# The generated tables preserve all existing enum indices; new entry is last.
orig=(d/'vulkan_thunks.c').read_text()
void_names=re.findall(r'    \(void \*\)thunk32_(\w+),',orig)
helper='\nNTSTATUS pw_vk_batch_dispatch(unsigned int code,void *args)\n{\n    switch(code)\n    {\n'+''.join('        case unix_'+n+': thunk32_'+n+'(args); return STATUS_SUCCESS;\n' for n in void_names)+'        default:\n#ifdef _WIN64\n            return __wine_unix_call_wow64_funcs[code](args);\n#else\n            return __wine_unix_call_funcs[code](args);\n#endif\n    }\n}\n'
edit('vulkan_thunks.c',lambda s:s.replace('};\nC_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == unix_count);','    pw_vk_batch_unix,\n};\nC_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == unix_count);')+helper)
# Generator emits exactly the same additive enum/table and void-call classifier.
def generator(s):
 for bits in [32,64]:
  needle='            f.write(f"    {func.unixlib_entry('+str(bits)+')},\\n")\n'
  s=once(s,needle,needle+'        f.write("    pw_vk_batch_unix,\\n")\n')
 s=once(s,'        f.write("    unix_count,\\n")','        f.write("    unix_pw_vk_batch,\\n")\n        f.write("    unix_count,\\n")')
 needle='        f.write("C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == unix_count);\\n")\n'
 addition='        f.write('+repr('\nNTSTATUS pw_vk_batch_dispatch(unsigned int code,void *args)\n{\n    switch(code)\n    {\n')+')\n'
 addition+='        for func in Type.all(Function, Function.needs_thunk):\n'
 addition+='            if func.is_perf_critical(): f.write(f'+repr('        case unix_{func.name}: thunk32_{func.name}(args); return STATUS_SUCCESS;\n')+')\n'
 addition+='        f.write('+repr('        default:\n#ifdef _WIN64\n            return __wine_unix_call_wow64_funcs[code](args);\n#else\n            return __wine_unix_call_funcs[code](args);\n#endif\n    }\n}\n')+')\n'
 s=once(s,needle,needle+addition);ast.parse(s);return s
edit('make_vulkan',generator)
print(d)
