/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Build once as PE32 client, once as PE64 DLL. */
#ifdef _WIN64
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
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
    return 0;
}
__declspec(dllexport) DWORD WINAPI PwD3D9ServiceMain(uint64_t *result)
{
    HANDLE thread; DWORD code; void *memory,*handler;
    if (!native_teb() || tls_value != 7) return 10;
    result[0]=(uintptr_t)NtCurrentTeb();
    memory=VirtualAlloc((void *)(uintptr_t)0x200000000,65536,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if (!memory) return 11;
    *(uint64_t *)memory=0x123456789abcdef0ULL;result[2]=(uintptr_t)memory;
    if ((uintptr_t)memory<=UINT32_MAX || *(uint64_t *)memory != 0x123456789abcdef0ULL) return 12;
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
static DWORD WINAPI guest_child(void *unused)
{
    (void)unused;
    return *(volatile LONG *)((char *)NtCurrentTeb()+0xfdc) ? 0 : 1;
}
static int guest_check(void)
{
    DWORD code=1; HANDLE thread=CreateThread(NULL,0,guest_child,NULL,0,NULL);
    if(!thread)return 1;
    WaitForSingleObject(thread,INFINITE);GetExitCodeThread(thread,&code);CloseHandle(thread);
    return code || !*(volatile LONG *)((char *)NtCurrentTeb()+0xfdc);
}
int main(int argc,char **argv)
{
    struct request req; query_fn query=(query_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"),"NtQueryInformationProcess");
    ULONG size; LONG status; unsigned i;
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
        printf("PW_NATIVE_DOMAIN iteration=%u status=%08lx size=%lu parent=%llx child=%llx high=%llx backend=%llx flags=%llu\n",i,status,size,req.result[0],req.result[1],req.result[2],req.result[4],req.result[3]);fflush(stdout);
        if(status || size!=592 || (req.result[3]&31)!=31 || req.result[2]<=UINT32_MAX || !req.result[1] || req.result[0]==req.result[1]) return 2;
    }
    return guest_check();
}

#endif
