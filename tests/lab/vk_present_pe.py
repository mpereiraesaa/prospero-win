#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Execute shared Present wrapper in both PE ABIs with a controlled raw thunk."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
for name in ('wine-build', 'prefix', 'output'):
    parser.add_argument('--' + name, type=Path, required=name == 'output')
parser.add_argument('--host-control', action='store_true', help='Scalar Win32 stubs; no Wine or GPU process.')
parser.add_argument('--compile-only', action='store_true', help='Build the PE fixtures without starting Wine.')
args = parser.parse_args()
if not args.host_control and not args.compile_only and (args.wine_build is None or args.prefix is None):
    parser.error('--wine-build and --prefix are required without --host-control')
root = Path(__file__).resolve().parents[2]
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)
files = ['wine/ps5/vulkan/pw_vk_present_pe.c', 'wine/ps5/vulkan/pw_vk_present_pe.h',
         'wine/ps5/vulkan/pw_vk_present_interval.h']
receipt = {'status': 'running', 'sources': {}, 'commands': [], 'console_accessed': False,
           'scope': ('Compile-only PE fixtures; no runtime acceptance.' if args.compile_only else 'Actual observer with scalar Win32 stubs; no synchronization/GPU acceptance.' if args.host_control else 'Actual PE32/PE64 wrapper/Win32 synchronization with controlled thunk/QPC; not actual Vulkan presentation or console cadence.')}
receipt['sources']['tests/lab/vk_present_pe.py'] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
receipt['sources']['wine/ps5/vulkan/pw_vk_batch_pe.c'] = hashlib.sha256((root / 'wine/ps5/vulkan/pw_vk_batch_pe.c').read_bytes()).hexdigest()
for name in files:
    source = root / name
    receipt['sources'][name] = hashlib.sha256(source.read_bytes()).hexdigest()
    shutil.copyfile(source, out / source.name)
win32 = '#include <windows.h>\n'
if args.host_control:
    win32 = r'''
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef int BOOL;typedef int32_t LONG,NTSTATUS;typedef unsigned long ULONG;
typedef unsigned long long UINT64;typedef struct {long long QuadPart;} LARGE_INTEGER;
typedef struct {int ready;} INIT_ONCE;typedef int CRITICAL_SECTION;
#define INIT_ONCE_STATIC_INIT {0}
#define CALLBACK
#define TRUE 1
static inline int InitOnceExecuteOnce(INIT_ONCE *o,BOOL (*fn)(INIT_ONCE *,void *,void **),void *p,void **c){if(!o->ready){if(!fn(o,p,c))return 0;o->ready=1;}return 1;}
static inline void InitializeCriticalSection(CRITICAL_SECTION *c){*c=0;}
static inline void EnterCriticalSection(CRITICAL_SECTION *c){(void)c;}
static inline void LeaveCriticalSection(CRITICAL_SECTION *c){(void)c;}
static inline unsigned GetEnvironmentVariableA(const char *n,char *b,unsigned cap){const char *v=getenv(n);unsigned len=v?(unsigned)strlen(v):0;if(cap){unsigned take=len<cap-1?len:cap-1;if(take)memcpy(b,v,take);b[take]=0;}return len;}
static inline BOOL SetEnvironmentVariableA(const char *n,const char *v){return !setenv(n,v,1);}
'''
(out / 'vulkan_loader.h').write_text(win32 + r'''
#include <stdio.h>
#include <stdint.h>
typedef void *VkQueue;
enum {unix_vkQueuePresentKHR=1,unix_vkDestroyDevice,unix_other,unix_batch};
struct test_batch {unsigned code;void *args;NTSTATUS status;};
enum {VK_SUCCESS=0,VK_SUBOPTIMAL_KHR=1000001003,VK_ERROR_UNKNOWN=-13};
struct vkQueuePresentKHR_params {VkQueue queue;void *pPresentInfo;int result;};
NTSTATUS test_unix(unsigned int,void *);
BOOL test_counter(LARGE_INTEGER *);
BOOL test_frequency(LARGE_INTEGER *);
#define WINE_DEFAULT_DEBUG_CHANNEL(name)
#define WINE_MESSAGE(...) printf(__VA_ARGS__)
#define WINE_UNIX_CALL(code,args) test_unix(code,args)
#define QueryPerformanceCounter test_counter
#define QueryPerformanceFrequency test_frequency
''')
(out / 'fixture.c').write_text(r'''
#include "vulkan_loader.h"
#include "pw_vk_present_pe.h"
#include <assert.h>
static uint64_t tick;static unsigned raw_calls,qpc_calls;static LONG status;
static int result,counter_ok=1;
NTSTATUS test_unix(unsigned int code,void *args){raw_calls++;if(code==unix_batch){struct test_batch *b=args;b->status=status;if(b->code==unix_vkQueuePresentKHR&&!status)((struct vkQueuePresentKHR_params *)b->args)->result=result;return 0;}if(code==unix_vkQueuePresentKHR&&!status)((struct vkQueuePresentKHR_params *)args)->result=result;return status;}
BOOL test_counter(LARGE_INTEGER *out){qpc_calls++;out->QuadPart=tick;return counter_ok;}
BOOL test_frequency(LARGE_INTEGER *out){out->QuadPart=1000000;return TRUE;}
static void present(uintptr_t queue,uint64_t now,LONG st,int vk)
{
 struct vkQueuePresentKHR_params q={(void *)queue,NULL,0x12345678};unsigned before=raw_calls;
 tick=now;status=st;result=vk;assert(pw_vk_present_call(unix_vkQueuePresentKHR,&q)==st);
 assert(raw_calls==before+1);assert(q.result==(st?0x12345678:vk));
}
int main(int argc,char **argv)
{
 unsigned i;(void)argv;SetEnvironmentVariableA("PW_VK_BATCH_STATS",argc>1?"0":"1");
 assert(!pw_vk_present_call(unix_other,NULL)&&raw_calls==1&&!qpc_calls);
 present(1,0,0,VK_SUCCESS);present(1,25001,0,VK_SUCCESS);
 present(1,58002,0,VK_SUBOPTIMAL_KHR);present(1,58003,0,VK_ERROR_UNKNOWN);
 present(1,60000,5,VK_SUCCESS);present(1,70000,0,VK_SUCCESS);
 status=0;assert(!pw_vk_present_call(unix_vkDestroyDevice,NULL));
 present(1,100000,0,VK_SUCCESS);
 for(i=2;i<=9;i++)present(i,110000+i,0,VK_SUCCESS);
 present(1,200000,0,VK_SUCCESS);
 counter_ok=0;present(1,210000,0,VK_SUCCESS);counter_ok=1;
 present(1,220000,0,VK_SUCCESS);
 assert(raw_calls==20);assert(qpc_calls==(argc>1?0:18));
 /* Model the actual completed-tail contract: one batch thunk then observation.
  * The observer must not invoke a second thunk, including on nested failure. */
 pw_vk_present_forget();
 for(i=0;i<4;i++){
  struct vkQueuePresentKHR_params q={(void *)1,NULL,0x12345678};
  struct test_batch b={unix_vkQueuePresentKHR,&q,0};unsigned before=raw_calls;
  tick=300000+i*30001;status=i==2?5:0;result=VK_SUCCESS;
  assert(!test_unix(unix_batch,&b));pw_vk_present_observe(b.code,b.args,b.status);
  assert(raw_calls==before+1&&q.result==(status?0x12345678:VK_SUCCESS));
 }
 /* Failed batched destroy preserves history; successful one clears it. */
 struct test_batch destroy={unix_vkDestroyDevice,NULL,0};
 status=5;assert(!test_unix(unix_batch,&destroy));pw_vk_present_observe(destroy.code,destroy.args,destroy.status);
 present(1,500000,0,VK_SUCCESS);
 status=0;assert(!test_unix(unix_batch,&destroy));pw_vk_present_observe(destroy.code,destroy.args,destroy.status);
 present(1,600000,0,VK_SUCCESS);
 assert(raw_calls==28&&qpc_calls==(argc>1?0:24));
 puts("PW_PRESENT_PE PASS");return 0;
}
''')
def run(command, name, env=None):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=120, env=env)
    (out / (name + '.log')).write_text(result.stdout + result.stderr)
    receipt['commands'].append({'command': list(map(str, command)), 'name': name, 'exit': result.returncode})
    if result.returncode:
        raise RuntimeError(f'{name} failed: {result.returncode}')
    return result.stdout
try:
    env = os.environ.copy()
    if not args.host_control and not args.compile_only:
        env.update(WINEPREFIX=str(args.prefix.resolve()), WINEDEBUG='-all', WINEDLLOVERRIDES='mscoree,mshtml=')
    for arch in (('host',) if args.host_control else ('i686', 'x86_64')):
        exe = out / (arch + '.exe')
        compiler = ['cc', '-D_POSIX_C_SOURCE=200809L'] if arch == 'host' else [arch + '-w64-mingw32-gcc']
        run([*compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
             out / 'fixture.c', out / 'pw_vk_present_pe.c', '-o', exe], 'compile-' + arch)
        if args.compile_only:
            continue
        for disabled in (False, True):
            command = [exe] if arch == 'host' else [args.wine_build.resolve() / 'loader/wine', exe]
            text = run([*command, *(['disabled'] if disabled else [])],
                       arch + ('-disabled' if disabled else '-enabled'), env)
            assert 'PW_PRESENT_PE PASS' in text, text
            rows = [line for line in text.splitlines() if 'PW_VK_PRESENT_INTERVAL' in line]
            if disabled:
                assert not rows, rows
                continue
            assert len(rows) == 23, rows
            batch_rows = rows[17:]
            rows = rows[:17]
            assert 'valid=1 interval_us=30001 intervals=1' in batch_rows[1], batch_rows
            assert 'result=-13 status=5 valid=0' in batch_rows[2], batch_rows
            assert 'valid=0 interval_us=0' in batch_rows[3], batch_rows
            assert 'valid=1 interval_us=109997' in batch_rows[4], batch_rows
            assert 'valid=0 interval_us=0 intervals=0' in batch_rows[5], batch_rows
            assert sum('dropped=1 reason=queue_capacity' in line for line in rows) == 1, rows
            assert 'valid=1 interval_us=25001 intervals=1 max_us=25001 over25ms=1 over33ms=0 over50ms=0' in rows[1], rows[1]
            assert 'valid=1 interval_us=33001 intervals=2 max_us=33001 over25ms=2 over33ms=1 over50ms=0' in rows[2], rows[2]
            assert 'result=-13 status=5 valid=0' in rows[4], rows[4]
            assert 'valid=1 interval_us=100000 intervals=1 max_us=100000 over25ms=1 over33ms=1 over50ms=1' in rows[-2], rows[-2]
            assert 'valid=0 interval_us=0 intervals=0' in rows[-1], rows[-1]
    receipt['status'] = 'compiled-only' if args.compile_only else 'pass'
except Exception:
    receipt['status'] = 'failed'
    raise
finally:
    (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(receipt['status'].upper() + ' ' + str(out / 'receipt.json'))
