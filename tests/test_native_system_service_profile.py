import pathlib, subprocess, tempfile, unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
PATCH = ROOT / "wine/patches/0612-wow64-opt-in-named-system-service-profile.patch"

class NativeSystemServiceProfile(unittest.TestCase):
    def test_actual_patch_counters_and_disabled_path(self):
        added = "\n".join(line[1:] for line in PATCH.read_text().splitlines() if line.startswith("+") and not line.startswith("+++"))
        added = added[:added.index("    pw_syscall_profile(num, args);")]
        prelude = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>
#include <stdarg.h>
#include <assert.h>
typedef uint64_t ULONGLONG;
typedef uintptr_t ULONG_PTR;
typedef unsigned long ULONG;
typedef unsigned UINT, DWORD, BOOL;
typedef wchar_t WCHAR;
typedef struct {unsigned short Length,MaximumLength; WCHAR *Buffer;} UNICODE_STRING;
typedef struct {int done;} RTL_RUN_ONCE;
#define RTL_RUN_ONCE_INIT {0}
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define WINAPI
#define TRUE 1
#define FALSE 0
#define HEAP_ZERO_MEMORY 1
#define ALL_SYSCALLS32 SYSCALL_ENTRY(0,NtYieldExecution,0) SYSCALL_ENTRY(1,NtProtectVirtualMemory,0)
struct peb {void *ProcessHeap;};
struct teb {struct peb *Peb;struct {void *UniqueThread;} ClientId;};
static struct peb peb;
static struct teb threads[65], *current = threads;
static int enabled, fail_alloc, allocations, reports, recursion;
static struct teb *NtCurrentTeb(void){return current;}
static void RtlInitUnicodeString(UNICODE_STRING *s,const WCHAR *p){s->Buffer=(WCHAR *)p;}
static int RtlQueryEnvironmentVariable_U(void *e,UNICODE_STRING *n,UNICODE_STRING *v){(void)e;(void)n;if(!enabled)return 1;v->Buffer[0]='1';v->Length=sizeof(WCHAR);return 0;}
static int RtlRunOnceExecuteOnce(RTL_RUN_ONCE *o,ULONG (*fn)(RTL_RUN_ONCE *,void *,void **),void *a,void **c){if(!o->done){o->done=1;fn(o,a,c);}return 0;}
static void *InterlockedCompareExchangePointer(void **p,void *v,void *expected){void *old=*p;if(old==expected)*p=v;return old;}
static void pw_syscall_profile(UINT num,UINT *args);
static void *RtlAllocateHeap(void *heap,unsigned flags,size_t n){(void)heap;(void)flags;allocations++;if(recursion){UINT a[3]={0};pw_syscall_profile(0,a+2);}return fail_alloc?NULL:calloc(1,n);}
static void message(const char *fmt,...){va_list a;va_start(a,fmt);if(strstr(fmt,"PW_NATIVE_SYSCALL version="))reports++;va_end(a);}
#define MESSAGE message
"""
        main = r"""
int main(int argc,char **argv){
 unsigned i;UINT stack[3]={0x1111,0x12345678,0};
 assert(argc==2); enabled=atoi(argv[1]);fail_alloc=enabled==2;recursion=1;
 for(i=0;i<65;i++){threads[i].Peb=&peb;threads[i].ClientId.UniqueThread=(void *)(uintptr_t)(i+1);}
 pw_syscall_profile(0x4000,stack+2);
 assert(allocations==0);
 for(i=0;i<32768;i++)pw_syscall_profile(i&1,stack+2);
 if(!enabled){assert(allocations==0 && reports==0);return 0;}
 if(fail_alloc){assert(allocations==1 && reports==0);return 0;}
 assert(allocations==1 && reports==2);
 assert(pw_syscall_slots[0].data->counts[0]==16384);
 assert(pw_syscall_slots[0].data->counts[1]==16384);
 assert(pw_syscall_slots[0].data->caller_counts[0]==16384);
 assert(pw_syscall_slots[0].data->callers[0]==0x12345678);
 for(i=1;i<65;i++){current=threads+i;pw_syscall_profile(0,stack+2);}
 assert(allocations==64);
 for(i=0;i<64;i++)free(pw_syscall_slots[i].data);
 return 0;
}
"""
        with tempfile.TemporaryDirectory() as td:
            source=pathlib.Path(td)/"profile.c"; binary=pathlib.Path(td)/"profile"
            source.write_text(prelude+added+main)
            subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-fsanitize=undefined",str(source),"-o",str(binary)],check=True)
            for mode in ("0","1","2"):
                subprocess.run([str(binary),mode],check=True)

if __name__=="__main__": unittest.main()
