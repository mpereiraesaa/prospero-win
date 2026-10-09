/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Build once as PE32 client, once as PE64 DLL. */
#ifdef _WIN64
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdint.h>
struct session_info { ULONG version, size; uint64_t token; };
static uint64_t session_token(void)
{
    typedef LONG (WINAPI *query_fn)(HANDLE,ULONG,void *,ULONG,ULONG *);
    query_fn query=(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryInformationThread");
    struct session_info info={1,sizeof(info),0};ULONG returned=0;HANDLE duplicate;
    if(!query || sizeof(info)!=16)return 0;
    if(query(GetCurrentThread(),0x50570002,&info,sizeof(info)-1,&returned)!=(LONG)0xc0000004 || returned!=16)return 0;
    info.version=2;
    if(query(GetCurrentThread(),0x50570002,&info,sizeof(info),NULL)!=(LONG)0xc000000d)return 0;
    info.version=1;
    if(!DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS))return 0;
    LONG status=query(duplicate,0x50570002,&info,sizeof(info),NULL);CloseHandle(duplicate);
    if(status!=(LONG)0xc00000bb || info.token)return 0;
    if(query(GetCurrentThread(),0x50570002,&info,sizeof(info),&returned) || returned!=16)return 0;
    return info.token;
}
static __thread unsigned tls_value = 7;
static DWORD tls_index;
static volatile LONG exceptions;
static int native_teb(void) { return *(volatile LONG *)((char *)NtCurrentTeb() + 0x180c) == 0; }
static LONG CALLBACK exception_handler(EXCEPTION_POINTERS *p)
{
    if (p->ExceptionRecord->ExceptionCode != 0xe0425057) return EXCEPTION_CONTINUE_SEARCH;
    InterlockedIncrement(&exceptions);
    return EXCEPTION_CONTINUE_EXECUTION;
}
static DWORD WINAPI child(void *arg)
{
    uint64_t *result=arg;
    if (!native_teb() || tls_value != 7 || TlsGetValue(tls_index)) return 1;
    tls_value=19;
    if (!TlsSetValue(tls_index,(void *)(uintptr_t)0x12345678)) return 2;
    RaiseException(0xe0425057,0,0,NULL);
    if (tls_value != 19 || TlsGetValue(tls_index)!=(void *)(uintptr_t)0x12345678) return 3;
    result[1]=(uintptr_t)NtCurrentTeb();
    result[6]=session_token();
    return result[6] ? 0 : 4;
}
static volatile LONG suspend_done;
static DWORD WINAPI suspend_parent(void *arg)
{
    HANDLE parent=arg;
    Sleep(50);
    for(unsigned i=0;i<8;i++){
        if(SuspendThread(parent)==(DWORD)-1)return 31;
        CONTEXT context={0};context.ContextFlags=CONTEXT_CONTROL|CONTEXT_INTEGER;
        if(!GetThreadContext(parent,&context)){ResumeThread(parent);InterlockedExchange(&suspend_done,1);return 32;}
        if(context.Rip<=UINT32_MAX||context.Rsp<=UINT32_MAX){ResumeThread(parent);InterlockedExchange(&suspend_done,1);return 35;}
        fprintf(stderr,"PW_NATIVE_SUSPEND iteration=%u rip=%llx rsp=%llx\n",i,(unsigned long long)context.Rip,(unsigned long long)context.Rsp);fflush(stderr);
        if(ResumeThread(parent)==(DWORD)-1)return 33;
        Sleep(10);
    }
    InterlockedExchange(&suspend_done,1);return 0;
}
__attribute__((used,noinline)) static DWORD suspend_probe(void)
    {
        HANDLE parent,worker;
        if(!DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&parent,0,FALSE,DUPLICATE_SAME_ACCESS))return 30;
        suspend_done=0;worker=CreateThread(NULL,0,suspend_parent,parent,0,NULL);if(!worker)return 34;
        while(!suspend_done)MsgWaitForMultipleObjectsEx(0,NULL,5,QS_ALLINPUT,MWMO_ALERTABLE);
        WaitForSingleObject(worker,INFINITE);DWORD result_code;GetExitCodeThread(worker,&result_code);
        CloseHandle(worker);CloseHandle(parent);if(result_code)return result_code;return 0;
    }

__attribute__((naked)) static DWORD high_stack_call(void *top __attribute__((unused)))
{
    __asm__ volatile("push %rbp; push %rbx; push %r12; push %r13; mov %rsp,%r12; mov %rcx,%rsp; and $-16,%rsp; sub $32,%rsp; movabs $0x1234567887654321,%rbx; movabs $0x8765432112345678,%r13; call suspend_probe; movabs $0x1234567887654321,%r10; cmp %r10,%rbx; jne 1f; movabs $0x8765432112345678,%r10; cmp %r10,%r13; je 2f; 1: mov $36,%eax; 2: mov %r12,%rsp; pop %r13; pop %r12; pop %rbx; pop %rbp; ret");
}

__declspec(dllexport) DWORD WINAPI PwD3D9ServiceMain(uint64_t *result)
{
    HANDLE thread; DWORD code; void *memory,*handler;
    if (!native_teb() || tls_value != 7) return 10;
    result[0]=(uintptr_t)NtCurrentTeb();
    result[5]=session_token();if(!result[5])return 22;
    {
        typedef void *(WINAPI *alloc2_fn)(HANDLE,void *,SIZE_T,ULONG,ULONG,MEM_EXTENDED_PARAMETER *,ULONG);
        alloc2_fn alloc2=(void *)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"),"VirtualAlloc2");
        MEM_ADDRESS_REQUIREMENTS address={0};MEM_EXTENDED_PARAMETER parameter={0};
        address.LowestStartingAddress=(void *)(uintptr_t)0x100000000;
        address.Alignment=65536;
        parameter.Type=MemExtendedParameterAddressRequirements;parameter.Pointer=&address;
        if(!alloc2)return 11;
        address.HighestEndingAddress=(void *)(uintptr_t)UINT32_MAX;
        memory=alloc2(GetCurrentProcess(),NULL,1048576,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE,&parameter,1);
        if(memory){VirtualFree(memory,0,MEM_RELEASE);return 20;}
        if(GetLastError()!=ERROR_INVALID_PARAMETER)return 21;
        address.HighestEndingAddress=NULL;
        memory=alloc2(GetCurrentProcess(),NULL,1048576,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE,&parameter,1);
        if(!memory){printf("PW_NATIVE_DOMAIN_ALLOC_FAILED error=%lu lowest=100000000\n",GetLastError());fflush(stdout);return 11;}
    }
    *(uint64_t *)memory=0x123456789abcdef0ULL;result[2]=(uintptr_t)memory;
    if ((uintptr_t)memory<=UINT32_MAX || *(uint64_t *)memory != 0x123456789abcdef0ULL) return 12;
    {
        NT_TIB *tib=(NT_TIB *)NtCurrentTeb();void *old_base=tib->StackBase,*old_limit=tib->StackLimit;
        tib->StackBase=(char *)memory+1048576;tib->StackLimit=memory;
        DWORD suspended=high_stack_call(tib->StackBase);
        tib->StackBase=old_base;tib->StackLimit=old_limit;
        if(suspended)return suspended;
    }
    VirtualFree(memory,0,MEM_RELEASE);
    if ((tls_index=TlsAlloc())==TLS_OUT_OF_INDEXES) return 13;
    tls_value=23;TlsSetValue(tls_index,(void *)(uintptr_t)0x87654321);
    handler=AddVectoredExceptionHandler(1,exception_handler);
    if (!handler) return 14;
    thread=CreateThread(NULL,0,child,result,0,NULL);
    if (!thread) return 15;
    WaitForSingleObject(thread,INFINITE);GetExitCodeThread(thread,&code);CloseHandle(thread);
    RaiseException(0xe0425057,0,0,NULL);RemoveVectoredExceptionHandler(handler);
    if (code || exceptions != 2 || tls_value != 23 || TlsGetValue(tls_index)!=(void *)(uintptr_t)0x87654321) return 16;
    if(result[5]!=result[6])return 23;
    TlsFree(tls_index);result[3]=31;
    {
        WCHAR path[260]; HMODULE backend; IDirect3D9 *d3d;
        IDirect3D9 *(WINAPI *factory)(UINT);
        if (GetEnvironmentVariableW(L"PW_BRIDGE_BACKEND64",path,260)) {
            backend=LoadLibraryW(path); if (!backend) return 17;
            factory=(void *)GetProcAddress(backend,"Direct3DCreate9"); if(!factory) return 18;
            d3d=factory(D3D_SDK_VERSION);if(!d3d)return 19;
            result[4]=(uintptr_t)d3d; IDirect3D9_Release(d3d);
            FreeLibrary(backend);result[3]|=32;
        }
    }
    return 0;
}

#else
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
struct request { ULONG version,size; WCHAR path[260]; uint64_t result[8]; };
typedef LONG (WINAPI *query_fn)(HANDLE,ULONG,void *,ULONG,ULONG *);
static volatile LONG guest_done;
static DWORD WINAPI guest_wait(void *arg){(void)arg;while(!guest_done)Sleep(1);return 0;}
static int guest_suspend(void)
{
    guest_done=0;HANDLE worker=CreateThread(NULL,0,guest_wait,NULL,0,NULL);if(!worker)return 1;
    Sleep(10);if(SuspendThread(worker)==(DWORD)-1)return 1;
    CONTEXT c={0};c.ContextFlags=CONTEXT_CONTROL|CONTEXT_INTEGER;
    BOOL valid=GetThreadContext(worker,&c)&&c.Eip&&c.Esp;
    ResumeThread(worker);InterlockedExchange(&guest_done,1);WaitForSingleObject(worker,INFINITE);CloseHandle(worker);return !valid;
}
static DWORD WINAPI guest_child(void *unused)
{
    (void)unused;
    return *(volatile LONG *)((char *)NtCurrentTeb()+0xfdc) ? 0 : 1;
}
static int guest_check(void)
{
    typedef LONG (WINAPI *thread_query_fn)(HANDLE,ULONG,void *,ULONG,ULONG *);
    thread_query_fn query=(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryInformationThread");
    struct { ULONG version,size; uint64_t token; } info={1,16,0x1122334455667788ULL};
    if(!query || query(GetCurrentThread(),0x50570002,&info,sizeof(info),NULL)!=(LONG)0xc0000003 || info.token!=0x1122334455667788ULL)return 1;
    DWORD code=1; HANDLE thread=CreateThread(NULL,0,guest_child,NULL,0,NULL);
    if(!thread)return 1;
    WaitForSingleObject(thread,INFINITE);GetExitCodeThread(thread,&code);CloseHandle(thread);
    return code || guest_suspend() || !*(volatile LONG *)((char *)NtCurrentTeb()+0xfdc);
}
int main(int argc,char **argv)
{
    struct request req; query_fn query=(query_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"),"NtQueryInformationProcess");
    ULONG size; LONG status; unsigned i; uint64_t previous_token=0;
    if(argc!=2 || sizeof(req)!=592 || !query) return 1;
    if(guest_check())return 3;
    memset(&req,0,sizeof(req));req.version=2;req.size=sizeof(req);
    if(query(GetCurrentProcess(),0x50570001,&req,sizeof(req),&size)!=(LONG)0xc000000d)return 4;
    if(query(GetCurrentProcess(),0x50570001,&req,sizeof(req)-1,&size)!=(LONG)0xc0000004)return 5;
    req.version=1;
    MultiByteToWideChar(CP_UTF8,0,"Z:/pw-native-domain-missing/service.dll",-1,req.path,260);
    for(i=0;req.path[i];i++)if(req.path[i]=='/')req.path[i]=0x5c;
    if(query(GetCurrentProcess(),0x50570001,&req,sizeof(req),&size)!=(LONG)0xc0000135)return 6;
    for(i=0;i<10;i++) {
        memset(&req,0,sizeof(req));req.version=1;req.size=sizeof(req);
        MultiByteToWideChar(CP_UTF8,0,argv[1],-1,req.path,260);
        status=query(GetCurrentProcess(),0x50570001,&req,sizeof(req),&size);
        printf("PW_NATIVE_DOMAIN iteration=%u status=%08lx size=%lu parent=%llx child=%llx high=%llx backend=%llx flags=%llu token=%llu child_token=%llu\n",i,status,size,req.result[0],req.result[1],req.result[2],req.result[4],req.result[3],req.result[5],req.result[6]);fflush(stdout);
        if(status || size!=592 || (req.result[3]&31)!=31 || req.result[2]<=UINT32_MAX || !req.result[1] || req.result[0]==req.result[1]) return 2;
        if(req.result[5]<=previous_token || req.result[5]!=req.result[6])return 7;
        previous_token=req.result[5];
    }
    return guest_check();
}

#endif
