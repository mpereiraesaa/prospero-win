/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
static HWND window;
static HANDLE notify_start,notify_done,stop_sender;
static IDirect3D9 *factory,*victim;
static unsigned deferred_releases;
static unsigned armed,callbacks,failures;
static HANDLE blocked_entered,blocked_resume;
static DWORD blocked_thread;
static unsigned serial_failure;
int pw_d3d9_session_test_serial_failure(void)
{
    if(!serial_failure)return 0;
    serial_failure=0;
    if(!PostMessageW(window,WM_APP,0,0)||!SetEvent(blocked_resume))failures++;
    return 1;
}
void pw_d3d9_session_test_callback(void)
{
    if(GetCurrentThreadId()==blocked_thread){SetEvent(blocked_entered);WaitForSingleObject(blocked_resume,30000);return;}
    if(!armed)return;
    armed=0;
    if(!PostMessageW(window,WM_APP,0,0)||!SetEvent(notify_start)||WaitForSingleObject(notify_done,30000)!=WAIT_OBJECT_0)failures++;
}
static DWORD WINAPI competing_call(void *unused)
{
    (void)unused;D3DCAPS9 caps;blocked_thread=GetCurrentThreadId();
    HRESULT hr=IDirect3D9_GetDeviceCaps(factory,0,D3DDEVTYPE_HAL,&caps);
    blocked_thread=0;return hr==S_OK?0:1;
}
static DWORD WINAPI sender(void *unused)
{
    (void)unused;HANDLE waits[]={notify_start,stop_sender};
    while(WaitForMultipleObjects(2,waits,FALSE,INFINITE)==WAIT_OBJECT_0){
        if(!SendNotifyMessageW(window,WM_APP+1,0,0))InterlockedIncrement((LONG *)&failures);
        SetEvent(notify_done);
    }
    return 0;
}
static LRESULT CALLBACK procedure(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam)
{
    if(message==WM_APP||message==WM_APP+1){
        D3DCAPS9 caps,before;memset(&caps,0xa5,sizeof(caps));before=caps;
        IDirect3D9_AddRef(factory);
        HRESULT hr=IDirect3D9_GetDeviceCaps(factory,0,D3DDEVTYPE_HAL,&caps);
        if(hr!=RPC_E_CANTCALLOUT_ININPUTSYNCCALL||memcmp(&caps,&before,sizeof(caps)))failures++;
        IDirect3D9 *nested=Direct3DCreate9(D3D_SDK_VERSION);
        if(nested){failures++;IDirect3D9_Release(nested);}
        IDirect3D9_Release(factory);
        if(victim){IDirect3D9 *released=victim;victim=NULL;if(IDirect3D9_Release(released))failures++;deferred_releases++;}
        callbacks++;return 0;
    }
    return DefWindowProcW(hwnd,message,wparam,lparam);
}
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL line=%d\n",__LINE__);return 1;}} while(0)
int wmain(int argc,WCHAR **argv)
{
    CHECK(argc==3);
    CHECK(SetEnvironmentVariableW(L"PW_D3D9_SERVICE64",argv[1]));
    CHECK(SetEnvironmentVariableW(L"PW_D3D9_BACKEND64",argv[2]));
    WNDCLASSEXW cls={.cbSize=sizeof(cls),.lpfnWndProc=procedure,.hInstance=GetModuleHandleW(NULL),.lpszClassName=L"PW_RPC_UI"};
    CHECK(RegisterClassExW(&cls));
    window=CreateWindowExW(0,cls.lpszClassName,L"RPC owner",0,0,0,1,1,HWND_MESSAGE,NULL,cls.hInstance,NULL);CHECK(window);
    notify_start=CreateEventW(NULL,FALSE,FALSE,NULL);notify_done=CreateEventW(NULL,FALSE,FALSE,NULL);stop_sender=CreateEventW(NULL,TRUE,FALSE,NULL);
    CHECK(notify_start&&notify_done&&stop_sender);
    HANDLE thread=CreateThread(NULL,0,sender,NULL,0,NULL);CHECK(thread);
    factory=Direct3DCreate9(D3D_SDK_VERSION);CHECK(factory);
    for(unsigned n=0;n<20;n++){
        unsigned before=callbacks;D3DCAPS9 caps;
        victim=Direct3DCreate9(D3D_SDK_VERSION);CHECK(victim);
        armed=1;HRESULT hr=IDirect3D9_GetDeviceCaps(factory,0,D3DDEVTYPE_HAL,&caps);armed=0;
        CHECK(hr==S_OK&&caps.MaxTextureWidth&&callbacks==before+2&&!failures&&!victim&&deferred_releases==n+1);
        CHECK(IDirect3D9_GetAdapterCount(factory)>0);
    }
    blocked_entered=CreateEventW(NULL,TRUE,FALSE,NULL);blocked_resume=CreateEventW(NULL,TRUE,FALSE,NULL);
    CHECK(blocked_entered&&blocked_resume);
    victim=Direct3DCreate9(D3D_SDK_VERSION);CHECK(victim);
    HANDLE competitor=CreateThread(NULL,0,competing_call,NULL,0,NULL);CHECK(competitor);
    CHECK(WaitForSingleObject(blocked_entered,30000)==WAIT_OBJECT_0);
    serial_failure=1;D3DCAPS9 failed_caps,before_caps;memset(&failed_caps,0xa5,sizeof(failed_caps));before_caps=failed_caps;
    CHECK(IDirect3D9_GetDeviceCaps(factory,0,D3DDEVTYPE_HAL,&failed_caps)==E_FAIL);
    CHECK(!memcmp(&failed_caps,&before_caps,sizeof(failed_caps))&&!victim&&deferred_releases==21&&!failures);
    CHECK(WaitForSingleObject(competitor,30000)==WAIT_OBJECT_0);
    DWORD competitor_result;CHECK(GetExitCodeThread(competitor,&competitor_result)&&!competitor_result);
    CloseHandle(competitor);CloseHandle(blocked_entered);CloseHandle(blocked_resume);
    CHECK(IDirect3D9_GetAdapterCount(factory)>0);
    /* WM_QUIT is retained for the outer application's message loop. */
    PostQuitMessage(37);CHECK(IDirect3D9_GetAdapterCount(factory)>0);
    MSG message;CHECK(PeekMessageW(&message,NULL,WM_QUIT,WM_QUIT,PM_REMOVE)&&message.wParam==37);
    CHECK(IDirect3D9_Release(factory)==0);factory=NULL;
    SetEvent(stop_sender);CHECK(WaitForSingleObject(thread,30000)==WAIT_OBJECT_0);
    CloseHandle(thread);CloseHandle(notify_start);CloseHandle(notify_done);CloseHandle(stop_sender);
    DestroyWindow(window);UnregisterClassW(cls.lpszClassName,cls.hInstance);
    printf("PW_RPC_UI owner_calls=20 callbacks=%u nested_rejected=%u quit=37 deferred=%u serial_failure=1 status=%u\n",callbacks,callbacks*2,deferred_releases,failures);
    return failures?1:0;
}
