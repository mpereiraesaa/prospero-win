#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Execute the CPU backend's real notification code with controlled VM replies."""
import pathlib
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
PRELUDE = r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef uintptr_t ULONG_PTR;
typedef size_t SIZE_T;
typedef uint64_t ULONGLONG;
typedef int BOOL, NTSTATUS;
#define WINAPI
#define TRUE 1
#define FALSE 0
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define MEM_FREE 0
#define MEM_COMMIT 1
#define MEM_MAPPED 2
#define MEM_IMAGE 3
#define MEM_PRIVATE 4
#define MemoryBasicInformation 0
typedef pthread_mutex_t RTL_SRWLOCK;
#define RTL_SRWLOCK_INIT PTHREAD_MUTEX_INITIALIZER
#define RtlAcquireSRWLockExclusive(p) assert(!pthread_mutex_lock(p))
#define RtlReleaseSRWLockExclusive(p) assert(!pthread_mutex_unlock(p))
typedef struct { void *BaseAddress, *AllocationBase; SIZE_T RegionSize; unsigned State, Type; } MEMORY_BASIC_INFORMATION;
static _Thread_local struct { struct { void *UniqueThread; } ClientId; } teb;
#define NtCurrentTeb() (&teb)
#define GetCurrentProcess() ((void *)-1)
enum { BASE=0x10000, PAGE=0x1000 };
static _Thread_local SIZE_T extent=2*PAGE;
static _Thread_local ULONG_PTR fail_at;
static _Thread_local unsigned view_type=MEM_MAPPED, malformed, reenter;
struct flush_record { ULONG_PTR address; SIZE_T size; };
static struct flush_record flushed[256];
static unsigned flush_count;
static pthread_mutex_t flush_lock=PTHREAD_MUTEX_INITIALIZER;
void WINAPI BTCpuNotifyUnmapViewOfSection(void *, BOOL, NTSTATUS);
static void flush(void *address, SIZE_T size)
{
    assert(!pthread_mutex_lock(&flush_lock));
    assert(flush_count<ARRAY_SIZE(flushed));
    flushed[flush_count++]=(struct flush_record){(ULONG_PTR)address,size};
    assert(!pthread_mutex_unlock(&flush_lock));
}
static NTSTATUS NtQueryVirtualMemory(void *process,void *address,int kind,
                                    MEMORY_BASIC_INFORMATION *info,SIZE_T bytes,void *returned)
{
    ULONG_PTR at=(ULONG_PTR)address;
    (void)process;(void)kind;(void)bytes;(void)returned;
    if(reenter) {
        SIZE_T original=extent;
        reenter=0;extent=4*PAGE;
        BTCpuNotifyUnmapViewOfSection((void *)(BASE+8),FALSE,0);
        BTCpuNotifyUnmapViewOfSection((void *)(BASE+8),TRUE,0);
        extent=original;
    }
    if(fail_at && at==fail_at)return -1;
    if(malformed==2) {
        *info=(MEMORY_BASIC_INFORMATION){(void *)(UINTPTR_MAX-1023),(void *)(UINTPTR_MAX-1023),2048,MEM_COMMIT,view_type};
        return 0;
    }
    if(at<BASE || at>=BASE+extent) {
        *info=(MEMORY_BASIC_INFORMATION){address,NULL,PAGE,MEM_FREE,0};return 0;
    }
    *info=(MEMORY_BASIC_INFORMATION){(void *)(at&~(ULONG_PTR)(PAGE-1)),(void *)BASE,malformed?0:PAGE,MEM_COMMIT,view_type};
    return 0;
}
'''
DRIVER = r'''
static void begin(unsigned thread,ULONG_PTR at,SIZE_T size)
{
    teb.ClientId.UniqueThread=(void *)(ULONG_PTR)thread;extent=size;
    BTCpuNotifyUnmapViewOfSection((void *)at,FALSE,0);
}
static void finish(unsigned thread,ULONG_PTR at,NTSTATUS status)
{
    teb.ClientId.UniqueThread=(void *)(ULONG_PTR)thread;
    BTCpuNotifyUnmapViewOfSection((void *)at,TRUE,status);
}
static void want(unsigned row,ULONG_PTR at,SIZE_T size)
{
    assert(row<flush_count && flushed[row].address==at && flushed[row].size==size);
}
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready=PTHREAD_COND_INITIALIZER;
static unsigned waiting;
static void *worker(void *argument)
{
    unsigned thread=(unsigned)(ULONG_PTR)argument;
    begin(thread,BASE+24,thread*PAGE);
    assert(!pthread_mutex_lock(&gate));
    waiting++;
    assert(!pthread_cond_broadcast(&ready));
    while(waiting<8)assert(!pthread_cond_wait(&ready,&gate));
    assert(!pthread_mutex_unlock(&gate));
    finish(thread,BASE+24,0);
    return NULL;
}
int main(int argc,char **argv)
{
    assert(argc==2);
    const char *which=argv[1];
    if(!strcmp(which,"multi-region") || !strcmp(which,"image")) {
        if(!strcmp(which,"image"))view_type=MEM_IMAGE;
        begin(1,BASE+24,3*PAGE);finish(1,BASE+24,0);want(0,BASE,3*PAGE);
    } else if(!strcmp(which,"partial-query-failure")) {
        fail_at=BASE+PAGE;begin(1,BASE+24,3*PAGE);finish(1,BASE+24,0);want(0,BASE+24,0);
    } else if(!strcmp(which,"private-or-free")) {
        view_type=MEM_PRIVATE;begin(1,BASE,2*PAGE);finish(1,BASE,0);want(0,BASE,0);
        view_type=MEM_MAPPED;begin(1,BASE+8*PAGE,2*PAGE);finish(1,BASE+8*PAGE,0);want(1,BASE+8*PAGE,0);
    } else if(!strcmp(which,"thread-interleave")) {
        begin(1,BASE+24,2*PAGE);begin(2,BASE+24,4*PAGE);
        finish(2,BASE+24,0);finish(1,BASE+24,0);want(0,BASE,4*PAGE);want(1,BASE,2*PAGE);
    } else if(!strcmp(which,"nested-same-address")) {
        begin(1,BASE+24,2*PAGE);begin(1,BASE+24,4*PAGE);
        finish(1,BASE+24,0);finish(1,BASE+24,0);want(0,BASE,4*PAGE);want(1,BASE,2*PAGE);
    } else if(!strcmp(which,"failed-inner-query")) {
        begin(1,BASE+24,2*PAGE);fail_at=BASE+24;begin(1,BASE+24,4*PAGE);
        finish(1,BASE+24,0);fail_at=0;finish(1,BASE+24,0);want(0,BASE+24,0);want(1,BASE,2*PAGE);
    } else if(!strcmp(which,"failed-unmap")) {
        begin(1,BASE,2*PAGE);finish(1,BASE,-1);assert(!flush_count);
        begin(1,BASE,4*PAGE);finish(1,BASE,0);want(0,BASE,4*PAGE);
    } else if(!strcmp(which,"saturation-and-recovery")) {
        for(unsigned i=0;i<65;i++)begin(i+1,BASE,2*PAGE);
        for(unsigned i=0;i<65;i++){finish(i+1,BASE,0);want(i,BASE,0);}
        begin(1,BASE,4*PAGE);finish(1,BASE,0);want(65,BASE,4*PAGE);
    } else if(!strcmp(which,"query-reentry")) {
        reenter=1;begin(1,BASE+24,2*PAGE);finish(1,BASE+24,0);want(0,BASE,4*PAGE);want(1,BASE,2*PAGE);
    } else if(!strcmp(which,"nonprogress") || !strcmp(which,"overflow")) {
        malformed=!strcmp(which,"overflow")?2:1;
        begin(1,BASE,2*PAGE);finish(1,BASE,0);want(0,BASE,0);
    } else if(!strcmp(which,"after-only")) {
        finish(1,BASE,0);want(0,BASE,0);
        begin(1,BASE,4*PAGE);finish(1,BASE,0);want(1,BASE,4*PAGE);
    } else if(!strcmp(which,"concurrent-threads")) {
        pthread_t threads[8];unsigned seen=0;
        for(unsigned i=0;i<8;i++)assert(!pthread_create(&threads[i],NULL,worker,(void *)(ULONG_PTR)(i+1)));
        for(unsigned i=0;i<8;i++)assert(!pthread_join(threads[i],NULL));
        assert(flush_count==8);
        for(unsigned i=0;i<8;i++) {
            assert(flushed[i].address==BASE && flushed[i].size>=PAGE && flushed[i].size<=8*PAGE);
            unsigned bit=1u<<(flushed[i].size/PAGE-1);assert(!(seen&bit));seen|=bit;
        }
        assert(seen==255);
    } else if(!strcmp(which,"ambiguous-address")) {
        begin(1,BASE,2*PAGE);begin(1,BASE+24,4*PAGE);
        finish(1,BASE,0);finish(1,BASE+24,0);want(0,BASE,0);want(1,BASE+24,0);
        begin(1,BASE,3*PAGE);finish(1,BASE,0);want(2,BASE,3*PAGE);
    } else if(!strcmp(which,"unknown-thread")) {
        begin(0,BASE,2*PAGE);finish(0,BASE,0);want(0,BASE,0);
        begin(1,BASE,3*PAGE);finish(1,BASE,0);want(1,BASE,3*PAGE);
    } else abort();
    puts(which);return 0;
}
'''


class UnmapNotifications(unittest.TestCase):
    def test_real_callback(self):
        compiler_command = shlex.split(os.environ.get("CC", "cc"))
        compiler = shutil.which(compiler_command[0])
        self.assertIsNotNone(compiler, "a C compiler is required for the callback regression")
        source = (ROOT / "wine/wowprospero/cpu.c").read_text()
        callback = source[source.index("struct pending_unmap\n"):source.index("\nstatic void raise_guest_exception")]
        cases = ["multi-region", "image", "partial-query-failure", "private-or-free",
                 "thread-interleave", "nested-same-address", "failed-inner-query", "failed-unmap",
                 "saturation-and-recovery", "query-reentry", "nonprogress", "overflow", "after-only",
                 "concurrent-threads", "ambiguous-address", "unknown-thread"]
        with tempfile.TemporaryDirectory(prefix="pw-unmap-test-") as directory:
            path = pathlib.Path(directory)
            (path / "test.c").write_text(PRELUDE + callback + DRIVER)
            flags = shlex.split(os.environ.get("CFLAGS", "-std=c11 -O1 -Wall -Wextra -Werror"))
            subprocess.run([compiler, *compiler_command[1:], *flags, "-pthread",
                            str(path / "test.c"), "-o", str(path / "test")], check=True, timeout=30)
            for case in cases:
                with self.subTest(case=case):
                    subprocess.run([str(path / "test"), case], check=True, timeout=5, stdout=subprocess.DEVNULL)


if __name__ == "__main__":
    unittest.main()
