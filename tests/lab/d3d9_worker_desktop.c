/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
static DWORD WINAPI worker(void *arg)
{
    HWND window=arg;
    /* First USER call on this worker: do not initialize GetDesktopWindow first. */
    HWND root=GetAncestor(window,GA_ROOT);
    RECT r={0};BOOL ok=GetClientRect(window,&r);
    printf("PW_WORKER_DESKTOP bits=%u root=%p window=%p rect=%ld,%ld,%ld,%ld ok=%d\n",
           (unsigned)(8*sizeof(void *)),root,window,r.left,r.top,r.right,r.bottom,ok);fflush(stdout);
    if(root!=window||!ok||r.right!=640||r.bottom!=480)return 1;
    if(GetAncestor(window,GA_ROOT)!=window)return 2;
    /* This invokes full lazy initialization after recognition-only caching. */
    HWND builtin=CreateWindowW(L"STATIC",L"worker builtin",WS_POPUP,0,0,32,32,NULL,NULL,GetModuleHandleW(NULL),NULL);
    if(!builtin)return 3;
    BOOL text=SetWindowTextW(builtin,L"initialized");WCHAR buffer[32]={0};
    int count=GetWindowTextW(builtin,buffer,32);DestroyWindow(builtin);
    return !text||count!=11||lstrcmpW(buffer,L"initialized");
}
static DWORD probe(void)
{
    WNDCLASSW cls={0};cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(NULL);cls.lpszClassName=L"PwWorkerDesktop";
    if(!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return 10;
    HWND window=CreateWindowW(cls.lpszClassName,L"worker desktop",WS_POPUP,0,0,640,480,NULL,NULL,cls.hInstance,NULL);
    if(!window)return 11;
    if(GetAncestor(window,GA_ROOT)!=window){DestroyWindow(window);return 12;}
    HANDLE thread=CreateThread(NULL,0,worker,window,0,NULL);DWORD code=13;
    if(thread){if(WaitForSingleObject(thread,10000)==WAIT_OBJECT_0)GetExitCodeThread(thread,&code);CloseHandle(thread);}
    DestroyWindow(window);UnregisterClassW(cls.lpszClassName,cls.hInstance);return code;
}
#ifdef _WIN64
__declspec(dllexport) DWORD WINAPI PwD3D9ServiceMain(uint64_t *result){DWORD code=probe();result[0]=code;return code;}
#else
struct request { ULONG version,size; WCHAR path[260]; uint64_t result[8]; };
typedef LONG (WINAPI *query_fn)(HANDLE,ULONG,void *,ULONG,ULONG *);
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    DWORD guest=probe();printf("PW_WORKER_GUEST status=%lu\n",guest);fflush(stdout);
    struct request r={0};r.version=1;r.size=sizeof(r);ULONG size=0;
    MultiByteToWideChar(CP_UTF8,0,argv[1],-1,r.path,260);
    query_fn query=(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryInformationProcess");
    if(!query)return 3;
    LONG status=query(GetCurrentProcess(),0x50570001,&r,sizeof(r),&size);
    printf("PW_WORKER_NATIVE status=%08lx result=%llu size=%lu\n",status,(unsigned long long)r.result[0],size);fflush(stdout);
    return guest||status||r.result[0]?1:0;
}
#endif
