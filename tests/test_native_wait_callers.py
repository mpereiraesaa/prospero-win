#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Execute the actual syscall diagnostic addition with deterministic Wine stubs."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
patch = (ROOT / 'wine/patches/0612-wow64-opt-in-named-system-service-profile.patch').read_text()
added = '\n'.join(line[1:] for line in patch.splitlines() if line.startswith('+') and not line.startswith('+++'))
added = added[:added.index('\n    pw_syscall_profile(num, args);')]
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdio.h>
typedef unsigned long long ULONGLONG;
typedef uint32_t ULONG, UINT;
typedef uintptr_t ULONG_PTR;
typedef int BOOL, RTL_RUN_ONCE;
typedef wchar_t WCHAR;
typedef struct { unsigned short Length, MaximumLength; WCHAR *Buffer; } UNICODE_STRING;
#define WINAPI
#define TRUE 1
#define FALSE 0
#define RTL_RUN_ONCE_INIT 0
#define HEAP_ZERO_MEMORY 1
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define ALL_SYSCALLS32 SYSCALL_ENTRY(0,NtProtectVirtualMemory,0) SYSCALL_ENTRY(1,NtWaitForAlertByThreadId,0) SYSCALL_ENTRY(2,NtClose,0)
static unsigned reports;
#define MESSAGE(...) do { ++reports; } while (0)
static struct { void *ProcessHeap; } peb;
static struct teb { struct { void *UniqueThread; } ClientId; __typeof__(peb) *Peb; } tebs[2];
static struct teb *current;
static struct teb *NtCurrentTeb(void) { return current; }
static void *InterlockedCompareExchangePointer(void **p, void *value, void *expected)
{ void *old=*p; if(old==expected)*p=value; return old; }
static void *RtlAllocateHeap(void *heap, unsigned flags, size_t size)
{ (void)heap; (void)flags; return calloc(1,size); }
static const WCHAR *profile_env, *wait_env;
static void RtlInitUnicodeString(UNICODE_STRING *s, const WCHAR *p)
{ s->Buffer=(WCHAR *)p; s->Length=wcslen(p)*sizeof(*p); }
static int RtlQueryEnvironmentVariable_U(void *env, UNICODE_STRING *name, UNICODE_STRING *value)
{
 const WCHAR *s=!wcscmp(name->Buffer,L"PW_NATIVE_SYSCALL_PROFILE")?profile_env:wait_env;
 size_t n; (void)env;
 if(!s)return 1;
 n=wcslen(s)*sizeof(*s); if(n>value->MaximumLength)return 2;
 memcpy(value->Buffer,s,n); value->Length=n; return 0;
}
static void RtlRunOnceExecuteOnce(RTL_RUN_ONCE *once,
 ULONG (*fn)(RTL_RUN_ONCE *,void *,void **),void *arg,void **ctx)
{ if(!*once){fn(once,arg,ctx);*once=1;} }
'''
checks = r'''
static void reset(const WCHAR *profile, const WCHAR *wait)
{
 for(unsigned i=0;i<ARRAY_SIZE(pw_syscall_slots);++i)free(pw_syscall_slots[i].data);
 memset(pw_syscall_slots,0,sizeof(pw_syscall_slots));
 pw_syscall_once=0;pw_syscall_enabled=pw_wait_enabled=0;pw_wait_tid=0;
 profile_env=profile;wait_env=wait;reports=0;
 tebs[0].ClientId.UniqueThread=(void *)0xac;tebs[0].Peb=&peb;
 tebs[1].ClientId.UniqueThread=(void *)0xb0;tebs[1].Peb=&peb;
 current=&tebs[0];
}
int main(void)
{
 UINT stack[]={0x1111,0x2222,0}, *args=stack+2;
 reset(NULL,L"0");pw_syscall_profile(1,args);assert(!pw_syscall_slots[0].data);
 reset(L"1",NULL);pw_syscall_profile(1,args);assert(!pw_syscall_slots[0].data->wait_counts[0]);
 const WCHAR *bad[]={L"",L"0xAC",L"-1",L"aZ",L"123456789",L"00ac "};
 for(unsigned i=0;i<ARRAY_SIZE(bad);++i){reset(L"1",bad[i]);pw_syscall_profile(1,args);assert(!pw_wait_enabled);}
 reset(L"1",L"00AC");pw_syscall_profile(1,args);
 struct pw_syscall_histogram *p=pw_syscall_slots[0].data;
 assert(p->wait_callers[0]==0x2222 && p->wait_counts[0]==1);
 pw_syscall_profile(1,args);assert(p->wait_counts[0]==2);
 pw_syscall_profile(0,args);assert(p->caller_counts[0]==1 && p->wait_counts[0]==2);
 current=&tebs[1];pw_syscall_profile(1,args);assert(!pw_syscall_slots[1].data->wait_counts[0]);
 reset(L"1",L"0");pw_syscall_profile(1,args);current=&tebs[1];pw_syscall_profile(1,args);
 assert(pw_syscall_slots[0].data->wait_counts[0]==1 && pw_syscall_slots[1].data->wait_counts[0]==1);
 reset(L"1",L"ac");
 for(unsigned i=0;i<65;++i){stack[1]=i;pw_syscall_profile(1,args);}
 p=pw_syscall_slots[0].data;assert(p->wait_counts[0]==1 && p->wait_callers[0]==0);
 assert(p->wait_counts[63]==1 && p->wait_other==1);
 stack[1]=5;pw_syscall_profile(1,args);assert(p->wait_counts[5]==2 && p->wait_other==1);
 for(unsigned i=66;i<16384;++i)pw_syscall_profile(2,args);
 assert(reports==67); /* header, two service rows, 64 wait caller rows */
 reset(NULL,NULL);
 puts("native wait callers: disabled, strict selection, independent threads, caller identity, overflow and report boundary passed");
 return 0;
}
'''
# The live implementation uses tsc only for logging; this fixture discards logs.
added = added.replace('    ULONGLONG tsc;', '    ULONGLONG tsc;').replace('    tsc = ((ULONGLONG)num << 32) | i;', '    tsc = ((ULONGLONG)num << 32) | i; (void)tsc;')
with tempfile.TemporaryDirectory(prefix='native-wait-callers-') as tmp:
    source = Path(tmp) / 'test.c'
    binary = Path(tmp) / 'test'
    source.write_text(harness + added + checks)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + shlex.split(os.environ.get('CFLAGS', '-O2 -Wall -Wextra -Werror')) + [str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

# Exercise the actual gate accounting functions with a deterministic clock and
# lock boundary. The report stub asserts that output happens after unlocking.
pe = (ROOT / 'wine/ps5/vulkan/pw_vk_batch_pe.c').read_text()
gate_decls = pe[pe.index('static BOOL gate_profile;'):pe.index('static UINT64 gate_clock(')]
gate_functions = pe[pe.index('static void enter(void)'):pe.index('static struct producer *producer(void)')]
gate_harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef int BOOL;
typedef unsigned long DWORD;
typedef unsigned long long UINT64;
#define TRUE 1
#define FALSE 0
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
static int gate, locked;
static DWORD owner, tid=1;
static unsigned depth, reports, clock_calls;
static UINT64 now;
static UINT64 gate_clock(void) { ++clock_calls; now+=10; return now; }
static DWORD GetCurrentThreadId(void) { return tid; }
static void EnterCriticalSection(int *g) { assert(g==&gate&&!locked);locked=1;now+=30; }
static void LeaveCriticalSection(int *g) { assert(g==&gate&&locked);locked=0; }
static void fatal(void) { abort(); }
#define WINE_MESSAGE(...) do { assert(!locked&&!owner&&!depth); ++reports; } while(0)
'''
gate_checks = r'''
int main(void)
{
 enter(); now+=100; leave();assert(!clock_calls&&!reports);
 gate_profile=TRUE;
 for(unsigned i=0;i<4096;i++){enter();now+=100;leave();}
 assert(reports==1&&gate_samples[0].entries==4096);
 assert(gate_samples[0].acquire_ticks==4096*40ULL);
 assert(gate_samples[0].hold_ticks==4096*110ULL);
 tid=2;enter();now+=7;leave();
 assert(gate_samples[1].tid==2&&gate_samples[1].entries==1);
 assert(gate_samples[1].acquire_ticks==40&&gate_samples[1].hold_ticks==17);
 assert(gate_samples[0].entries==4096);
 for(tid=3;tid<=65;tid++){enter();leave();}
 assert(gate_samples[63].tid==64&&gate_samples[63].entries==1);
 assert(!gate_active&&!locked&&!depth);
 puts("batch gate timing: disabled path, distinct thread totals, acquisition/hold intervals, capacity, reporting after unlock passed");
 return 0;
}
'''
# The log stub intentionally discards the copied sample after checking unlock.
gate_functions = gate_functions.replace('struct gate_sample sample={0};', 'struct gate_sample sample={0};(void)sample;')
with tempfile.TemporaryDirectory(prefix='batch-gate-timing-') as tmp:
    source = Path(tmp) / 'test.c'
    binary = Path(tmp) / 'test'
    source.write_text(gate_harness + gate_decls + gate_functions + gate_checks)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + shlex.split(os.environ.get('CFLAGS', '-O2 -Wall -Wextra -Werror')) + [str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
