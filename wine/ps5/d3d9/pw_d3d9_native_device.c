/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_session.h"
#include "../pw_d3d9_window_driver.h"
#include <d3d9.h>
#include <string.h>
#include <stdio.h>
struct pw_d3d9_native_device {
    IDirect3DDevice9 *device;
    HWND window;
    struct pw_d3d9_window_id guest,association;
    uint64_t sequence;
    int closed,destroyed,fullscreen,log_geometry,reset_preserved,reset_succeeded;
    unsigned implicit_phase;
    struct pw_d3d9_native_device *next;
};
static struct pw_d3d9_native_device *devices;
static int cleanup_failed;
static ATOM window_class;
static HINSTANCE module;
static ULONG_PTR (WINAPI *driver_call)(ULONG_PTR,ULONG_PTR,DWORD);
static const WCHAR class_name[]=L"PW_D3D9_NATIVE_DEVICE";
static int same(struct pw_d3d9_window_id a,struct pw_d3d9_window_id b)
{return a.epoch==b.epoch&&a.id==b.id&&a.generation==b.generation;}
static int null_id(struct pw_d3d9_window_id id)
{return !id.epoch&&!id.id&&!id.generation;}
static LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam)
{return DefWindowProcW(window,message,wparam,lparam);}
static int init_class(void)
{
    if(window_class)return 1;
    HMODULE win32u=GetModuleHandleW(L"win32u.dll");
    if(!win32u)return 0;
    driver_call=(void *)GetProcAddress(win32u,"NtUserCallTwoParam");
    if(!driver_call||!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(const WCHAR *)window_proc,&module))return 0;
    WNDCLASSEXW cls={.cbSize=sizeof(cls),.lpfnWndProc=window_proc,.hInstance=module,.lpszClassName=class_name};
    window_class=RegisterClassExW(&cls);return !!window_class;
}
static int driver(struct pw_d3d9_native_device *d,unsigned operation,struct pw_d3d9_window_driver_request *q)
{
    q->version=PW_D3D9_WINDOW_DRIVER_VERSION;q->size=sizeof(*q);q->operation=operation;
    q->guest=d->guest;q->service=(uintptr_t)d->window;q->id=d->association;
    return driver_call((ULONG_PTR)q,sizeof(*q),PW_D3D9_WINDOW_DRIVER_CALL)==PW_D3D9_WINDOW_OK;
}
static int mirror(struct pw_d3d9_native_device *d,int attach)
{
    struct pw_d3d9_window_driver_request q={0};
    if(!driver(d,attach?PW_D3D9_WINDOW_ATTACH:PW_D3D9_WINDOW_QUERY_STATE,&q))return 0;
    if(attach)d->association=q.id;
    if(!q.state.width||!q.state.height||d->sequence==UINT64_MAX)return 0;
    struct pw_d3d9_window_state guest_state=q.state;
    if(d->fullscreen){
        RECT rect;POINT origin={0,0};
        if(!GetClientRect(d->window,&rect)||!ClientToScreen(d->window,&origin))return 0;
        int64_t width=(int64_t)rect.right-rect.left,height=(int64_t)rect.bottom-rect.top;
        if(width<=0||height<=0||width>INT32_MAX||height>INT32_MAX)return 0;
        q.state.x=origin.x;q.state.y=origin.y;q.state.width=(uint32_t)width;q.state.height=(uint32_t)height;
        q.state.flags=(q.state.flags&PW_D3D9_WINDOW_FOCUSED)|PW_D3D9_WINDOW_FULLSCREEN;
        if(IsWindowVisible(d->window))q.state.flags|=PW_D3D9_WINDOW_VISIBLE;
    }
    if(d->log_geometry){
        fprintf(stderr,"PW_BRIDGE_GEOMETRY fullscreen=%d guest=%d,%d,%u,%u service=%d,%d,%u,%u\n",d->fullscreen,guest_state.x,guest_state.y,guest_state.width,guest_state.height,q.state.x,q.state.y,q.state.width,q.state.height);
        d->log_geometry=0;
    }
    q.sequence=++d->sequence;
    if(!driver(d,PW_D3D9_WINDOW_BEGIN,&q))return 0;
    BOOL applied=d->fullscreen||SetWindowPos(d->window,NULL,q.state.x,q.state.y,(int)q.state.width,(int)q.state.height,
                              SWP_NOACTIVATE|SWP_NOZORDER);
    if(applied&&!d->fullscreen)ShowWindow(d->window,q.state.flags&PW_D3D9_WINDOW_VISIBLE?SW_SHOWNOACTIVATE:SW_HIDE);
    q.hresult=applied?S_OK:E_FAIL;
    return driver(d,PW_D3D9_WINDOW_ACK,&q)&&applied;
}
int pw_d3d9_native_device_destroy(struct pw_d3d9_native_device *d)
{
    struct pw_d3d9_window_driver_request q={0};
    if(!d)return 1;
    if(d->association.id&&!d->closed){
        if(!driver(d,PW_D3D9_WINDOW_CLOSE,&q))goto retained;
        d->closed=1;
    }
    if(d->device){IDirect3DDevice9_Release(d->device);d->device=NULL;}
    if(d->window&&!d->destroyed){
        if(!DestroyWindow(d->window))goto retained;
        d->destroyed=1;
    }
    if(d->association.id){memset(&q,0,sizeof(q));if(!driver(d,PW_D3D9_WINDOW_DETACH,&q))goto retained;}
    struct pw_d3d9_native_device **link=&devices;
    while(*link!=d)link=&(*link)->next;
    *link=d->next;HeapFree(GetProcessHeap(),0,d);return 1;
 retained:
    cleanup_failed=1;return 0;
}
int pw_d3d9_native_device_shutdown(void)
{
    /* Retained contexts keep the module/registry/driver callbacks alive. Never
     * force-unload after an unexpected close or lease-drain failure. */
    while(devices){
        struct pw_d3d9_native_device *d=devices;
        while(d){struct pw_d3d9_native_device *next=d->next;pw_d3d9_native_device_destroy(d);d=next;}
        if(devices){
            OutputDebugStringA("PW_D3D9: retaining native module while device cleanup drains\n");
            MsgWaitForMultipleObjects(0,NULL,FALSE,100,QS_ALLINPUT);
            MSG message;while(PeekMessageW(&message,NULL,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        }
    }
    while(window_class){
        if(UnregisterClassW(class_name,module)){window_class=0;break;}
        cleanup_failed=1;
        OutputDebugStringA("PW_D3D9: retaining native module while window class drains\n");
        MsgWaitForMultipleObjects(0,NULL,FALSE,100,QS_ALLINPUT);
        MSG message;while(PeekMessageW(&message,NULL,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
    }
    return !cleanup_failed;
}
void *pw_d3d9_native_device_backend(struct pw_d3d9_native_device *d)
{return d?d->device:NULL;}
unsigned pw_d3d9_native_device_implicit_phase(struct pw_d3d9_native_device *d)
{return d->implicit_phase;}
void pw_d3d9_native_device_implicit_set_phase(struct pw_d3d9_native_device *d,unsigned phase)
{d->implicit_phase=phase;}
int pw_d3d9_native_device_reset_succeeded(struct pw_d3d9_native_device *d)
{return d->reset_succeeded;}
int pw_d3d9_native_device_reset_preserved(struct pw_d3d9_native_device *d)
{return d->reset_preserved;}
uintptr_t pw_d3d9_native_device_identity(struct pw_d3d9_native_device *d)
{
    IUnknown *identity=NULL;
    if(FAILED(IDirect3DDevice9_QueryInterface(d->device,&IID_IUnknown,(void **)&identity)))return 0;
    uintptr_t value=(uintptr_t)identity;IUnknown_Release(identity);return value;
}
static void native_parameters(D3DPRESENT_PARAMETERS *out,const struct pw_d3d9_present_parameters *q,HWND window)
{
    memset(out,0,sizeof(*out));
#define COPY_PP(name,native) out->native=q->name;
    PW_D3D9_PRESENT_FIELDS(COPY_PP)
#undef COPY_PP
    out->hDeviceWindow=null_id(q->window)?NULL:window;
}
static void reply_parameters(struct pw_d3d9_present_parameters *out,const D3DPRESENT_PARAMETERS *p,struct pw_d3d9_window_id guest)
{
#define COPY_PP(name,native) out->name=p->native;
    PW_D3D9_PRESENT_FIELDS(COPY_PP)
#undef COPY_PP
    out->window=p->hDeviceWindow?guest:(struct pw_d3d9_window_id){0};
}
void pw_d3d9_native_device_call(void *factory,struct pw_d3d9_native_device *device,
    const struct pw_d3d9_device_request *q,struct pw_d3d9_device_reply *r,struct pw_d3d9_native_device **created)
{
    memset(r,0,sizeof(*r));r->operation=q->operation;r->hresult=D3DERR_NOTAVAILABLE;r->parameters=q->parameters;*created=NULL;
    if(q->operation==PW_D3D9_DEVICE_CREATE){
        struct pw_d3d9_window_id guest=null_id(q->parameters.window)?q->focus_window:q->parameters.window;
        if(null_id(guest)||(!null_id(q->focus_window)&&!same(q->focus_window,guest))||!init_class())return;
        struct pw_d3d9_native_device *d=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*d));
        if(!d){r->hresult=E_OUTOFMEMORY;return;}
        d->next=devices;devices=d;
        d->guest=guest;d->window=CreateWindowExW(WS_EX_NOACTIVATE,class_name,L"D3D9 native presentation",WS_POPUP,0,0,1,1,NULL,NULL,module,NULL);
        if(!d->window||!mirror(d,1)){pw_d3d9_native_device_destroy(d);return;}
        D3DPRESENT_PARAMETERS p;native_parameters(&p,&q->parameters,d->window);
        r->hresult=IDirect3D9_CreateDevice((IDirect3D9 *)factory,q->adapter,q->device_type,d->window,q->behavior_flags,&p,&d->device);
        if(SUCCEEDED((HRESULT)r->hresult)&&!d->device)r->hresult=E_FAIL;
        if(p.hDeviceWindow&&p.hDeviceWindow!=d->window)r->hresult=E_FAIL;
        reply_parameters(&r->parameters,&p,guest);
        if(FAILED((HRESULT)r->hresult)){pw_d3d9_native_device_destroy(d);return;}
        d->fullscreen=!p.Windowed;d->log_geometry=1;
        if(!mirror(d,0)){r->hresult=D3DERR_DEVICELOST;pw_d3d9_native_device_destroy(d);return;}
        *created=d;return;
    }
    if(!device){r->hresult=D3DERR_INVALIDCALL;return;}
    if(q->operation==PW_D3D9_DEVICE_RESET){
        device->reset_preserved=1;device->reset_succeeded=0; /* No backend call on preflight failure. */
        if((!null_id(q->parameters.window)&&!same(q->parameters.window,device->guest)))return;
        if(!mirror(device,0)){r->hresult=D3DERR_DEVICELOST;return;}
        D3DPRESENT_PARAMETERS p;native_parameters(&p,&q->parameters,device->window);
        r->hresult=IDirect3DDevice9_Reset(device->device,&p);
        /* The pinned backend leaves owners intact only on early validation
         * failure. Capture this BEFORE a post-Reset mirror can change HRESULT. */
        device->reset_succeeded=SUCCEEDED((HRESULT)r->hresult);
        device->reset_preserved=!device->reset_succeeded &&
            IDirect3DDevice9_TestCooperativeLevel(device->device)==S_OK;
        if(SUCCEEDED((HRESULT)r->hresult)){device->fullscreen=!p.Windowed;device->log_geometry=1;if(!mirror(device,0))r->hresult=D3DERR_DEVICELOST;}
        if(p.hDeviceWindow&&p.hDeviceWindow!=device->window)r->hresult=E_FAIL;
        reply_parameters(&r->parameters,&p,device->guest);
    }else if(q->operation==PW_D3D9_DEVICE_PRESENT){
        if(!null_id(q->override_window)&&!same(q->override_window,device->guest))return;
        if(!mirror(device,0)){r->hresult=D3DERR_DEVICELOST;return;}
        RECT source={q->source.left,q->source.top,q->source.right,q->source.bottom};
        RECT dest={q->destination.left,q->destination.top,q->destination.right,q->destination.bottom};
        struct {RGNDATAHEADER header;RECT rects[PW_D3D9_DEVICE_MAX_DIRTY_RECTS];} dirty={0};
        dirty.header.dwSize=sizeof(dirty.header);dirty.header.iType=RDH_RECTANGLES;
        dirty.header.nCount=q->dirty_count;dirty.header.nRgnSize=q->dirty_count*sizeof(RECT);
        dirty.header.rcBound=(RECT){q->dirty_bounds.left,q->dirty_bounds.top,q->dirty_bounds.right,q->dirty_bounds.bottom};
        for(unsigned n=0;n<q->dirty_count;n++)dirty.rects[n]=(RECT){q->dirty[n].left,q->dirty[n].top,q->dirty[n].right,q->dirty[n].bottom};
        r->hresult=IDirect3DDevice9_Present(device->device,
            q->present_fields&PW_D3D9_PRESENT_SOURCE?&source:NULL,
            q->present_fields&PW_D3D9_PRESENT_DESTINATION?&dest:NULL,
            null_id(q->override_window)?NULL:device->window,
            q->present_fields&PW_D3D9_PRESENT_DIRTY?(RGNDATA *)&dirty:NULL);
    }
}
