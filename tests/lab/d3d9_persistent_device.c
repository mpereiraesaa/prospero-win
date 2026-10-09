/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../../wine/ps5/d3d9/pw_d3d9_session.h"
#include "../../wine/ps5/pw_d3d9_window_driver.h"
#include <d3d9.h>
#include <stdio.h>
struct context {WCHAR *service,*backend;struct pw_d3d9_window_id guest;int unavailable,fullscreen;};
static DWORD WINAPI run_device(void *parameter)
{
    struct context *c=parameter;struct pw_d3d9_session *session=NULL;struct pw_d3d9_object_ref factory;
    struct pw_d3d9_device_request q={0};struct pw_d3d9_device_reply r;
    HRESULT create=E_FAIL,reset=E_FAIL,present=E_FAIL;DWORD result=1;
    if(FAILED(pw_d3d9_session_open(c->service,c->backend,&session)))goto done;
    if(FAILED(pw_d3d9_session_create(session,D3D_SDK_VERSION,&factory)))goto done;
    q.operation=PW_D3D9_DEVICE_CREATE;q.device_type=D3DDEVTYPE_HAL;q.behavior_flags=D3DCREATE_SOFTWARE_VERTEXPROCESSING;
    q.focus_window=c->guest;q.parameters.window=c->guest;
    q.parameters.windowed=1;q.parameters.swap_effect=D3DSWAPEFFECT_DISCARD;
    q.parameters.format=D3DFMT_UNKNOWN;q.parameters.count=1;q.parameters.interval=D3DPRESENT_INTERVAL_IMMEDIATE;
    if(c->fullscreen){q.parameters.windowed=0;q.parameters.width=1920;q.parameters.height=1080;q.parameters.format=D3DFMT_A8R8G8B8;q.parameters.swap_effect=D3DSWAPEFFECT_FLIP;q.parameters.auto_depth_stencil=TRUE;q.parameters.depth_stencil_format=D3DFMT_D16;q.parameters.interval=D3DPRESENT_INTERVAL_DEFAULT;}
    /* Zero dimensions must derive from the mirrored guest window. */
    create=pw_d3d9_session_device(session,factory,&q,&r);
    if(FAILED(create)){
        if(c->unavailable&&create==D3DERR_NOTAVAILABLE&&!r.object.id&&r.parameters.windowed==(uint32_t)!c->fullscreen&&r.parameters.window.id==c->guest.id)result=0;
        goto done;
    }
    if(c->unavailable)goto done;
    struct pw_d3d9_object_ref device=r.object;
    if(!device.id||!r.parameters.width||!r.parameters.height)goto done;
    q=(struct pw_d3d9_device_request){.operation=PW_D3D9_DEVICE_RESET,.parameters=r.parameters};
    reset=pw_d3d9_session_device(session,device,&q,&r);if(FAILED(reset))goto done;
    q=(struct pw_d3d9_device_request){.operation=PW_D3D9_DEVICE_PRESENT};
    present=pw_d3d9_session_device(session,device,&q,&r);if(FAILED(present))goto done;
    if(FAILED(pw_d3d9_session_release(session,device)))goto done;
    if(FAILED(pw_d3d9_session_release(session,factory)))goto done;
    result=0;
 done:
    if(session&&FAILED(pw_d3d9_session_close(session)))result=1;
    printf("PW_PERSISTENT_DEVICE create=%08lx reset=%08lx present=%08lx status=%lu unavailable=%d\n",(DWORD)create,(DWORD)reset,(DWORD)present,result,c->unavailable);fflush(stdout);
    return result;
}
static LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam)
{return DefWindowProcW(window,message,wparam,lparam);}
int wmain(int argc,WCHAR **argv)
{
    if(argc<3||argc>5)return 2;
    int unavailable=0,fullscreen=0;
    for(int i=3;i<argc;i++){if(!wcscmp(argv[i],L"--expect-unavailable"))unavailable=1;else if(!wcscmp(argv[i],L"--fullscreen"))fullscreen=1;else return 2;}
    WNDCLASSEXW cls={.cbSize=sizeof(cls),.lpfnWndProc=window_proc,.hInstance=GetModuleHandleW(NULL),.lpszClassName=L"PW_DEVICE_GUEST"};
    if(!RegisterClassExW(&cls))return 3;
    HWND window=CreateWindowExW(0,cls.lpszClassName,L"D3D9 bridge device",WS_OVERLAPPEDWINDOW|WS_VISIBLE,20,20,640,480,NULL,NULL,cls.hInstance,NULL);
    if(!window)return 4;
    ULONG_PTR (WINAPI *call)(ULONG_PTR,ULONG_PTR,DWORD)=(void *)GetProcAddress(GetModuleHandleW(L"win32u.dll"),"NtUserCallTwoParam");
    struct pw_d3d9_guest_window_request guest={.version=1,.size=sizeof(guest),.operation=PW_D3D9_GUEST_REGISTER};
    if(!call||call((ULONG_PTR)window,(ULONG_PTR)&guest,PW_D3D9_GUEST_WINDOW_CALL)!=PW_D3D9_WINDOW_OK)return 5;
    struct context context={argv[1],argv[2],guest.id,unavailable,fullscreen};DWORD result=0;
    for(unsigned cycle=0;cycle<3;cycle++){
        HANDLE worker=CreateThread(NULL,0,run_device,&context,0,NULL);
        if(!worker)return 6;
        for(;;){
            DWORD wait=MsgWaitForMultipleObjects(1,&worker,FALSE,60000,QS_ALLINPUT);
            if(wait==WAIT_OBJECT_0)break;
            if(wait!=WAIT_OBJECT_0+1)return 7;
            MSG message;while(PeekMessageW(&message,NULL,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        }
        GetExitCodeThread(worker,&result);CloseHandle(worker);
        if(result)break;
    }
    guest.operation=PW_D3D9_GUEST_UNREGISTER;
    if(call((ULONG_PTR)window,(ULONG_PTR)&guest,PW_D3D9_GUEST_WINDOW_CALL)!=PW_D3D9_WINDOW_OK)result=8;
    DestroyWindow(window);UnregisterClassW(cls.lpszClassName,cls.hInstance);return (int)result;
}
