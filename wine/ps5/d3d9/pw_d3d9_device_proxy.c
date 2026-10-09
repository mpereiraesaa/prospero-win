/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_device_proxy.h"
#ifdef PW_D3D9_ENABLE_METHODS
#include "pw_d3d9_device_methods.h"
#endif
#ifdef PW_D3D9_ENABLE_RESOURCE
#include "pw_d3d9_buffer_proxy.h"
#endif
#ifdef PW_D3D9_ENABLE_PROGRAM
#include "pw_d3d9_program_proxy.h"
#endif
#ifdef PW_D3D9_ENABLE_TEXTURE
#include "pw_d3d9_texture_proxy.h"
#endif
#ifdef PW_D3D9_ENABLE_CURSOR
#include "pw_d3d9_cursor.h"
#endif
#ifdef PW_D3D9_ENABLE_QUERY
#include "pw_d3d9_query_proxy.h"
#endif
#ifdef PW_D3D9_ENABLE_STATEBLOCK
#include "pw_d3d9_stateblock_client.h"
#endif
#ifdef PW_D3D9_ENABLE_OBJECT_GETTER
#include "pw_d3d9_device_object_methods.h"
#endif
#ifdef PW_D3D9_ENABLE_UP
#include "pw_d3d9_up_client.h"
#endif
#include "pw_d3d9_inventory.h"
#include "../pw_d3d9_window_driver.h"
#include <stdio.h>
#include <string.h>
struct guest_window {
    HWND guest,helper;
    DWORD owner;
    unsigned references,closing;
    struct pw_d3d9_window_id id;
    struct guest_window *next;
};
struct device_proxy {
    IDirect3DDevice9 iface;
    LONG references,failed;
    IDirect3D9 *parent;
    struct pw_d3d9_session *session;
    struct pw_d3d9_object_ref remote;
    struct guest_window *window;
    D3DDEVICE_CREATION_PARAMETERS creation;
    struct pw_d3d9_deferred cleanup;
};
static SRWLOCK windows_lock=SRWLOCK_INIT;
static struct guest_window *windows;
static ATOM helper_class;
static INIT_ONCE helper_once=INIT_ONCE_STATIC_INIT;
static HINSTANCE module;
static const WCHAR helper_name[]=L"PW_D3D9_GUEST_OWNER";
static ULONG_PTR (WINAPI *driver_call)(ULONG_PTR,ULONG_PTR,DWORD);
#define CLEANUP_WINDOW (WM_APP+0x39)
static LRESULT CALLBACK helper_proc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam)
{
    if(message==WM_NCCREATE){SetWindowLongPtrW(hwnd,GWLP_USERDATA,(LONG_PTR)((CREATESTRUCTW *)lparam)->lpCreateParams);return TRUE;}
    if(message==CLEANUP_WINDOW){
        struct guest_window *w=(void *)GetWindowLongPtrW(hwnd,GWLP_USERDATA);
        struct pw_d3d9_guest_window_request q={.version=1,.size=sizeof(q),.operation=PW_D3D9_GUEST_UNREGISTER,.id=w->id};
        ULONG_PTR result=driver_call((ULONG_PTR)w->guest,(ULONG_PTR)&q,PW_D3D9_GUEST_WINDOW_CALL);
        if(result!=PW_D3D9_WINDOW_OK&&result!=PW_D3D9_WINDOW_STALE)return 0;
        return DestroyWindow(hwnd);
    }
    return DefWindowProcW(hwnd,message,wparam,lparam);
}
static BOOL CALLBACK init_helper(INIT_ONCE *once,void *parameter,void **context)
{
    (void)once;(void)parameter;(void)context;
    driver_call=(void *)GetProcAddress(GetModuleHandleW(L"win32u.dll"),"NtUserCallTwoParam");
    if(!driver_call||!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(const WCHAR *)helper_proc,&module))return FALSE;
    WNDCLASSEXW cls={.cbSize=sizeof(cls),.lpfnWndProc=helper_proc,.hInstance=module,.lpszClassName=helper_name};
    helper_class=RegisterClassExW(&cls);return !!helper_class;
}
static HRESULT acquire_window(HWND hwnd,struct guest_window **out)
{
    DWORD pid=0,owner=GetWindowThreadProcessId(hwnd,&pid);*out=NULL;
    if(!owner||owner!=GetCurrentThreadId()||pid!=GetCurrentProcessId())return D3DERR_INVALIDCALL;
    AcquireSRWLockExclusive(&windows_lock);
    for(struct guest_window *w=windows;w;w=w->next)if(w->guest==hwnd){
        if(w->closing){ReleaseSRWLockExclusive(&windows_lock);return D3DERR_NOTAVAILABLE;}
        w->references++;*out=w;ReleaseSRWLockExclusive(&windows_lock);return S_OK;
    }
    ReleaseSRWLockExclusive(&windows_lock);
    if(!InitOnceExecuteOnce(&helper_once,init_helper,NULL,NULL))return D3DERR_NOTAVAILABLE;
    struct guest_window *w=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*w));if(!w)return E_OUTOFMEMORY;
    w->guest=hwnd;w->owner=owner;w->references=1;
    struct pw_d3d9_guest_window_request q={.version=1,.size=sizeof(q),.operation=PW_D3D9_GUEST_REGISTER};
    if(driver_call((ULONG_PTR)hwnd,(ULONG_PTR)&q,PW_D3D9_GUEST_WINDOW_CALL)!=PW_D3D9_WINDOW_OK){HeapFree(GetProcessHeap(),0,w);return D3DERR_NOTAVAILABLE;}
    w->id=q.id;
    w->helper=CreateWindowExW(0,helper_name,L"bridge owner",0,0,0,0,0,HWND_MESSAGE,NULL,module,w);
    if(!w->helper){q.operation=PW_D3D9_GUEST_UNREGISTER;driver_call((ULONG_PTR)hwnd,(ULONG_PTR)&q,PW_D3D9_GUEST_WINDOW_CALL);HeapFree(GetProcessHeap(),0,w);return E_OUTOFMEMORY;}
    AcquireSRWLockExclusive(&windows_lock);w->next=windows;windows=w;ReleaseSRWLockExclusive(&windows_lock);*out=w;return S_OK;
}
static int release_window(struct guest_window *w,struct pw_d3d9_session *session)
{
    AcquireSRWLockExclusive(&windows_lock);
    if(--w->references){ReleaseSRWLockExclusive(&windows_lock);return 1;}
    w->closing=1;ReleaseSRWLockExclusive(&windows_lock);
    if(!SendMessageW(w->helper,CLEANUP_WINDOW,0,0)){
        pw_d3d9_session_cancel(session);pw_d3d9_session_join(session);
        if(!SendMessageW(w->helper,CLEANUP_WINDOW,0,0))return 0;
    }
    AcquireSRWLockExclusive(&windows_lock);struct guest_window **link=&windows;
    while(*link!=w)link=&(*link)->next;
    *link=w->next;ReleaseSRWLockExclusive(&windows_lock);HeapFree(GetProcessHeap(),0,w);return 1;
}
void pw_d3d9_device_proxy_detach(void)
{if(helper_class&&!windows)UnregisterClassW(helper_name,module);}
static struct device_proxy *device(IDirect3DDevice9 *iface){return (struct device_proxy *)iface;}
static ULONG WINAPI addref(IDirect3DDevice9 *iface){return (ULONG)InterlockedIncrement(&device(iface)->references);}
static void cleanup_device(void *parameter)
{
    struct device_proxy *d=parameter;
    if(FAILED(pw_d3d9_session_release(d->session,d->remote))){pw_d3d9_session_cancel(d->session);pw_d3d9_session_join(d->session);}
    if(!release_window(d->window,d->session)){
        OutputDebugStringA("PW_D3D9: guest registration cleanup failed; retaining proxy ownership\n");return;
    }
    IDirect3D9 *parent=d->parent;HeapFree(GetProcessHeap(),0,d);IDirect3D9_Release(parent);
}
static ULONG WINAPI release(IDirect3DDevice9 *iface)
{
    struct device_proxy *d=device(iface);ULONG refs=(ULONG)InterlockedDecrement(&d->references);
    if(!refs){
        HRESULT hr=pw_d3d9_session_release(d->session,d->remote);
        if(hr==RPC_E_CANTCALLOUT_ININPUTSYNCCALL){
            d->cleanup.function=cleanup_device;d->cleanup.context=d;
            if(SUCCEEDED(pw_d3d9_session_defer(d->session,&d->cleanup)))return 0;
        }
        if(FAILED(hr)){pw_d3d9_session_cancel(d->session);pw_d3d9_session_join(d->session);}
        if(!release_window(d->window,d->session)){OutputDebugStringA("PW_D3D9: retaining failed owner cleanup\n");return 0;}
        IDirect3D9 *parent=d->parent;HeapFree(GetProcessHeap(),0,d);IDirect3D9_Release(parent);
    }
    return refs;
}
static HRESULT WINAPI query(IDirect3DDevice9 *iface,REFIID iid,void **out)
{
    if(!out)return E_POINTER;
    *out=NULL;
    if(!IsEqualGUID(iid,&IID_IUnknown)&&!IsEqualGUID(iid,&IID_IDirect3DDevice9))return E_NOINTERFACE;
    addref(iface);*out=iface;return S_OK;
}
static void unsupported(IDirect3DDevice9 *iface,unsigned slot,const char *name)
{
    char text[160];snprintf(text,sizeof(text),"PW_D3D9 unsupported device slot=%u method=%s\n",slot,name);
    InterlockedExchange(&device(iface)->failed,1);OutputDebugStringA(text);
}
#define FAIL_HRESULT D3DERR_NOTAVAILABLE
#define FAIL_ULONG 0
#define FAIL_UINT 0
#define FAIL_BOOL FALSE
#define FAIL_float 0.0f
#define FAIL_void ((void)0)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define STUB(iface,slot,name,ret,args,policy) static ret WINAPI unsupported_##name args {unsupported(self,slot,#name);return FAIL_##ret;}
PW_D3D9_METHODS_IDirect3DDevice9(STUB)
#undef STUB
#pragma GCC diagnostic pop
static IDirect3DDevice9Vtbl vtable;
static INIT_ONCE vtable_once=INIT_ONCE_STATIC_INIT;
static HRESULT WINAPI get_parent(IDirect3DDevice9 *iface,IDirect3D9 **out)
{if(!out)return D3DERR_INVALIDCALL;*out=device(iface)->parent;IDirect3D9_AddRef(*out);return S_OK;}
static HRESULT WINAPI get_creation(IDirect3DDevice9 *iface,D3DDEVICE_CREATION_PARAMETERS *out)
{if(!out)return D3DERR_INVALIDCALL;*out=device(iface)->creation;return S_OK;}
static HRESULT WINAPI get_caps(IDirect3DDevice9 *iface,D3DCAPS9 *out)
{
    addref(iface);struct device_proxy *d=device(iface);
    HRESULT hr=IDirect3D9_GetDeviceCaps(d->parent,d->creation.AdapterOrdinal,d->creation.DeviceType,out);
    release(iface);return hr;
}
static int parameters_to_wire(struct pw_d3d9_present_parameters *out,const D3DPRESENT_PARAMETERS *p,struct guest_window *window)
{
    if(p->hDeviceWindow&&p->hDeviceWindow!=window->guest)return 0;
#define COPY_PP(name,native) out->name=p->native;
    PW_D3D9_PRESENT_FIELDS(COPY_PP)
#undef COPY_PP
    out->window=p->hDeviceWindow?window->id:(struct pw_d3d9_window_id){0};return 1;
}
static int parameters_from_wire(D3DPRESENT_PARAMETERS *out,const struct pw_d3d9_present_parameters *p,struct guest_window *window)
{
    if(p->window.id&&(p->window.id!=window->id.id||p->window.epoch!=window->id.epoch||p->window.generation!=window->id.generation))return 0;
#define COPY_PP(name,native) out->native=p->name;
    PW_D3D9_PRESENT_FIELDS(COPY_PP)
#undef COPY_PP
    out->hDeviceWindow=p->window.id?window->guest:NULL;return 1;
}
static HRESULT WINAPI reset(IDirect3DDevice9 *iface,D3DPRESENT_PARAMETERS *parameters)
{
    if(!parameters)return D3DERR_INVALIDCALL;
    struct device_proxy *d=device(iface);struct pw_d3d9_device_request q={.operation=PW_D3D9_DEVICE_RESET};struct pw_d3d9_device_reply r={0};
    if(d->failed)return D3DERR_NOTAVAILABLE;
    if(!parameters_to_wire(&q.parameters,parameters,d->window))return D3DERR_NOTAVAILABLE;
    addref(iface);HRESULT hr=pw_d3d9_session_device(d->session,d->remote,&q,&r);
    if(r.operation==q.operation&&!parameters_from_wire(parameters,&r.parameters,d->window)){
        hr=E_FAIL;pw_d3d9_session_cancel(d->session);pw_d3d9_session_join(d->session);
    }
    release(iface);return hr;
}
static HRESULT WINAPI present(IDirect3DDevice9 *iface,const RECT *source,const RECT *destination,HWND override,const RGNDATA *dirty)
{
    struct device_proxy *d=device(iface);struct pw_d3d9_device_request q={.operation=PW_D3D9_DEVICE_PRESENT};struct pw_d3d9_device_reply r={0};
    if(d->failed)return D3DERR_NOTAVAILABLE;
    if(override&&override!=d->window->guest)return D3DERR_NOTAVAILABLE;
    if(override)q.override_window=d->window->id;
    if(source){q.present_fields|=PW_D3D9_PRESENT_SOURCE;q.source=(struct pw_d3d9_rect){source->left,source->top,source->right,source->bottom};}
    if(destination){q.present_fields|=PW_D3D9_PRESENT_DESTINATION;q.destination=(struct pw_d3d9_rect){destination->left,destination->top,destination->right,destination->bottom};}
    if(dirty){
        if(dirty->rdh.dwSize!=sizeof(RGNDATAHEADER)||dirty->rdh.iType!=RDH_RECTANGLES||dirty->rdh.nCount>PW_D3D9_DEVICE_MAX_DIRTY_RECTS||dirty->rdh.nRgnSize!=dirty->rdh.nCount*sizeof(RECT))return D3DERR_INVALIDCALL;
        q.present_fields|=PW_D3D9_PRESENT_DIRTY;q.dirty_count=dirty->rdh.nCount;
        q.dirty_bounds=(struct pw_d3d9_rect){dirty->rdh.rcBound.left,dirty->rdh.rcBound.top,dirty->rdh.rcBound.right,dirty->rdh.rcBound.bottom};
        for(unsigned n=0;n<q.dirty_count;n++){const RECT *r=(const RECT *)dirty->Buffer+n;q.dirty[n]=(struct pw_d3d9_rect){r->left,r->top,r->right,r->bottom};}
    }
    addref(iface);HRESULT hr=pw_d3d9_session_device(d->session,d->remote,&q,&r);release(iface);return hr;
}
#if defined(PW_D3D9_ENABLE_METHODS)||defined(PW_D3D9_ENABLE_RESOURCE)||defined(PW_D3D9_ENABLE_PROGRAM)||defined(PW_D3D9_ENABLE_TEXTURE)||defined(PW_D3D9_ENABLE_STATEBLOCK)||defined(PW_D3D9_ENABLE_QUERY)
static void fail_device(IDirect3DDevice9 *iface,HRESULT hr)
{(void)hr;struct device_proxy *d=device(iface);InterlockedExchange(&d->failed,1);pw_d3d9_session_cancel(d->session);}
#endif
#if defined(PW_D3D9_ENABLE_RESOURCE)||defined(PW_D3D9_ENABLE_PROGRAM)||defined(PW_D3D9_ENABLE_TEXTURE)||defined(PW_D3D9_ENABLE_STATEBLOCK)||defined(PW_D3D9_ENABLE_QUERY)
static HRESULT release_object(IDirect3DDevice9 *iface,struct pw_d3d9_object_ref ref)
{addref(iface);HRESULT hr=pw_d3d9_session_release(device(iface)->session,ref);release(iface);return hr;}
static HRESULT defer_object(IDirect3DDevice9 *iface,struct pw_d3d9_deferred *cleanup)
{return pw_d3d9_session_defer(device(iface)->session,cleanup);}
#endif
#ifdef PW_D3D9_ENABLE_RESOURCE
static HRESULT resource_call(IDirect3DDevice9 *iface,struct pw_d3d9_object_ref ref,const struct pw_d3d9_resource_request *q,struct pw_d3d9_resource_reply *r)
{
    struct device_proxy *d=device(iface);if(d->failed)return D3DERR_NOTAVAILABLE;
    if(!ref.id&&!ref.generation)ref=d->remote;
    addref(iface);HRESULT hr=pw_d3d9_session_resource(d->session,ref,q,r);release(iface);return hr;
}
#endif
#ifdef PW_D3D9_ENABLE_TEXTURE
static HRESULT texture_call(IDirect3DDevice9 *iface,struct pw_d3d9_object_ref ref,const struct pw_d3d9_texture_request *q,struct pw_d3d9_texture_reply *r)
{
    struct device_proxy *d=device(iface);if(d->failed)return D3DERR_NOTAVAILABLE;
    if(!ref.id&&!ref.generation)ref=d->remote;
    struct pw_d3d9_texture_reply decoded={0};
    addref(iface);HRESULT hr=pw_d3d9_session_texture(d->session,ref,q,&decoded);
    if(SUCCEEDED(hr)&&q->operation==PW_D3D9_TEXTURE_CONTAINER&&decoded.container_kind==PW_D3D9_KIND_DEVICE&&
       (decoded.object.id!=d->remote.id||decoded.object.generation!=d->remote.generation)){
        hr=E_FAIL;fail_device(iface,hr);
    }else if(decoded.operation==q->operation)*r=decoded;
    release(iface);return hr;
}
#endif
#ifdef PW_D3D9_ENABLE_CURSOR
static HRESULT cursor_call(IDirect3DDevice9 *iface,const struct pw_d3d9_cursor_request *q,struct pw_d3d9_cursor_reply *r)
{
    struct device_proxy *d=device(iface);if(d->failed)return D3DERR_NOTAVAILABLE;
    addref(iface);HRESULT hr=pw_d3d9_session_cursor(d->session,d->remote,q,r);release(iface);return hr;
}
#endif
#ifdef PW_D3D9_ENABLE_QUERY
static HRESULT query_call(IDirect3DDevice9 *iface,struct pw_d3d9_object_ref ref,const struct pw_d3d9_query_request *q,struct pw_d3d9_query_reply *r)
{
    struct device_proxy *d=device(iface);if(d->failed)return D3DERR_NOTAVAILABLE;
    if(!ref.id&&!ref.generation)ref=d->remote;
    addref(iface);HRESULT hr=pw_d3d9_session_query(d->session,ref,q,r);release(iface);return hr;
}
#endif
#ifdef PW_D3D9_ENABLE_STATEBLOCK
static HRESULT stateblock_call(IDirect3DDevice9 *iface,struct pw_d3d9_object_ref ref,const struct pw_d3d9_stateblock_request *q,struct pw_d3d9_stateblock_reply *r)
{
    struct device_proxy *d=device(iface);if(d->failed)return D3DERR_NOTAVAILABLE;
    if(!ref.id&&!ref.generation)ref=d->remote;
    addref(iface);HRESULT hr=pw_d3d9_session_stateblock(d->session,ref,q,r);release(iface);return hr;
}
#endif
#ifdef PW_D3D9_ENABLE_PROGRAM
static HRESULT program_call(IDirect3DDevice9 *iface,const struct pw_d3d9_program_request *q,struct pw_d3d9_program_reply *r)
{
    struct device_proxy *d=device(iface);if(d->failed)return D3DERR_NOTAVAILABLE;
    addref(iface);HRESULT hr=pw_d3d9_session_program(d->session,d->remote,q,r);release(iface);return hr;
}
static HRESULT program_query(IDirect3DDevice9 *iface,struct pw_d3d9_object_ref ref,uint32_t kind,void *output,UINT *size)
{
    if(!size)return D3DERR_INVALIDCALL;
    struct device_proxy *d=device(iface);if(d->failed)return D3DERR_NOTAVAILABLE;
    addref(iface);UINT capacity=*size;unsigned char *copy=NULL;HRESULT hr;
    struct pw_d3d9_program_query_request q={.operation=PW_D3D9_PROGRAM_SIZE,.kind=kind,.capacity=capacity};struct pw_d3d9_program_query_reply r={0};
    hr=pw_d3d9_session_program_query(d->session,ref,&q,&r);
    if(FAILED(hr)){if(r.operation==q.operation)*size=r.size;goto done;}
    if(!output){*size=r.size;goto done;}
    uint32_t total=r.total,bytes=kind==7?total:(capacity<total?capacity:total),offset=0,returned_size=capacity;
    copy=HeapAlloc(GetProcessHeap(),0,bytes?bytes:1);if(!copy){hr=E_OUTOFMEMORY;goto done;}
    do{
        q=(struct pw_d3d9_program_query_request){.operation=PW_D3D9_PROGRAM_READ,.kind=kind,.capacity=capacity,.offset=offset,.count=bytes-offset>4096?4096:bytes-offset};
        memset(&r,0,sizeof(r));hr=pw_d3d9_session_program_query(d->session,ref,&q,&r);
        if(FAILED(hr)){if(r.operation==q.operation)*size=r.size;goto done;}
        if(r.total!=total||r.offset!=offset||r.count!=q.count){hr=E_FAIL;fail_device(iface,hr);goto done;}
        memcpy(copy+offset,r.data,r.count);offset+=r.count;returned_size=r.size;
    }while(offset<bytes);
    SIZE_T written=0;
    if(bytes&&(!WriteProcessMemory(GetCurrentProcess(),output,copy,bytes,&written)||written!=bytes)){hr=D3DERR_INVALIDCALL;goto done;}
    *size=returned_size;
 done:
    if(copy)HeapFree(GetProcessHeap(),0,copy);
    release(iface);return hr;
}
#endif
#ifdef PW_D3D9_ENABLE_METHODS
static HRESULT command_call(IDirect3DDevice9 *iface,const struct pw_d3d9_command *q)
{
    struct device_proxy *d=device(iface);if(d->failed)return D3DERR_NOTAVAILABLE;
    addref(iface);HRESULT hr=pw_d3d9_session_command(d->session,d->remote,q);release(iface);return hr;
}
static HRESULT getter_call(IDirect3DDevice9 *iface,const struct pw_d3d9_getter_request *q,struct pw_d3d9_getter_reply *r)
{
    struct device_proxy *d=device(iface);if(d->failed)return D3DERR_NOTAVAILABLE;
    addref(iface);HRESULT hr=pw_d3d9_session_getter(d->session,d->remote,q,r);
    if(FAILED(hr)&&hr!=RPC_E_CANTCALLOUT_ININPUTSYNCCALL&&(q->method==4||q->method==15||q->method==78||q->method==80))fail_device(iface,hr);
    release(iface);return hr;
}
static HRESULT resolve_object(IDirect3DDevice9 *iface,IUnknown *local,uint32_t kind,struct pw_d3d9_object_ref *ref)
{
    if(!ref)return E_POINTER;
    *ref=(struct pw_d3d9_object_ref){0};if(!local)return S_OK;
#ifdef PW_D3D9_ENABLE_RESOURCE
    if(kind==3||kind==4)return pw_d3d9_buffer_proxy_resolve(iface,local,kind,ref);
#endif
#ifdef PW_D3D9_ENABLE_PROGRAM
    if(kind>=7&&kind<=9)return pw_d3d9_program_proxy_resolve(iface,local,kind,ref);
#endif
#ifdef PW_D3D9_ENABLE_TEXTURE
    if(kind==5||kind==6)return pw_d3d9_texture_proxy_resolve(iface,local,kind,ref);
#endif
    return D3DERR_INVALIDCALL;
}
#endif
#ifdef PW_D3D9_ENABLE_OBJECT_GETTER
static HRESULT object_getter_call(IDirect3DDevice9 *iface,const struct pw_d3d9_object_getter_request *q,struct pw_d3d9_object_getter_reply *r)
{
    struct device_proxy *d=device(iface);if(d->failed)return D3DERR_NOTAVAILABLE;
    addref(iface);HRESULT hr=pw_d3d9_session_object_getter(d->session,d->remote,q,r);release(iface);return hr;
}
static HRESULT wrap_object(IDirect3DDevice9 *iface,uint32_t kind,struct pw_d3d9_object_ref ref,void **out)
{
    if(kind==3||kind==4)return pw_d3d9_buffer_proxy_wrap(iface,kind,ref,out);
    if(kind==5||kind==6)return pw_d3d9_texture_proxy_wrap(iface,kind,ref,0,out);
    if(kind>=7&&kind<=9)return pw_d3d9_program_proxy_wrap(iface,kind,ref,out);
    if(out)*out=NULL;
    fail_device(iface,E_NOINTERFACE);return E_NOINTERFACE;
}
#endif
#ifdef PW_D3D9_ENABLE_UP
static HRESULT up_call(IDirect3DDevice9 *iface,const struct pw_d3d9_up_request *q,struct pw_d3d9_up_reply *r)
{
    struct device_proxy *d=device(iface);if(d->failed)return D3DERR_NOTAVAILABLE;
    addref(iface);HRESULT hr=pw_d3d9_session_up(d->session,d->remote,q,r);release(iface);return hr;
}
#endif
static BOOL CALLBACK init_vtable(INIT_ONCE *once,void *parameter,void **context)
{
    (void)once;(void)parameter;(void)context;
#define INIT(iface,slot,name,ret,args,policy) vtable.name=unsupported_##name;
    PW_D3D9_METHODS_IDirect3DDevice9(INIT)
#undef INIT
    vtable.QueryInterface=query;vtable.AddRef=addref;vtable.Release=release;
    vtable.GetDirect3D=get_parent;vtable.GetCreationParameters=get_creation;vtable.GetDeviceCaps=get_caps;
    vtable.Reset=reset;vtable.Present=present;
#ifdef PW_D3D9_ENABLE_METHODS
    const struct pw_d3d9_device_methods_ops methods={command_call,getter_call,fail_device,resolve_object};pw_d3d9_device_methods_install(&vtable,&methods);
#endif
#ifdef PW_D3D9_ENABLE_RESOURCE
    const struct pw_d3d9_buffer_proxy_ops buffers={resource_call,release_object,defer_object,fail_device};
    if(FAILED(pw_d3d9_buffer_proxy_install(&vtable,&buffers)))return FALSE;
#endif
#ifdef PW_D3D9_ENABLE_PROGRAM
    const struct pw_d3d9_program_proxy_ops programs={program_call,program_query,release_object,defer_object,fail_device};pw_d3d9_program_proxy_install(&vtable,&programs);
#endif
#ifdef PW_D3D9_ENABLE_TEXTURE
    const struct pw_d3d9_texture_proxy_ops textures={texture_call,release_object,defer_object,fail_device};pw_d3d9_texture_proxy_install(&vtable,&textures);
#endif
#ifdef PW_D3D9_ENABLE_CURSOR
    const struct pw_d3d9_cursor_ops cursor={cursor_call,resolve_object,fail_device};pw_d3d9_cursor_install(&vtable,&cursor);
#endif
#ifdef PW_D3D9_ENABLE_QUERY
    const struct pw_d3d9_query_proxy_ops queries={query_call,release_object,defer_object,fail_device};pw_d3d9_query_proxy_install(&vtable,&queries);
#endif
#ifdef PW_D3D9_ENABLE_STATEBLOCK
    const struct pw_d3d9_stateblock_client_ops blocks={stateblock_call,release_object,defer_object,fail_device};pw_d3d9_stateblock_client_install(&vtable,&blocks);
#endif
#ifdef PW_D3D9_ENABLE_OBJECT_GETTER
    const struct pw_d3d9_device_object_methods_ops objects={object_getter_call,wrap_object,fail_device};pw_d3d9_device_object_methods_install(&vtable,&objects);
#endif
#ifdef PW_D3D9_ENABLE_UP
    const struct pw_d3d9_up_client_ops up={up_call,fail_device};pw_d3d9_up_client_install(&vtable,&up);
#endif
    return TRUE;
}
HRESULT pw_d3d9_device_proxy_create(IDirect3D9 *parent,struct pw_d3d9_session *session,
    struct pw_d3d9_object_ref parent_ref,UINT adapter,D3DDEVTYPE type,HWND focus,DWORD flags,D3DPRESENT_PARAMETERS *parameters,IDirect3DDevice9 **out)
{
    if(!out)return D3DERR_INVALIDCALL;
    *out=NULL;
    if(!parameters)return D3DERR_INVALIDCALL;
    HWND guest=parameters->hDeviceWindow?parameters->hDeviceWindow:focus;
    if(!guest||(focus&&focus!=guest))return D3DERR_NOTAVAILABLE;
    if(!InitOnceExecuteOnce(&vtable_once,init_vtable,NULL,NULL))return E_FAIL;
    struct device_proxy *d=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*d));if(!d)return E_OUTOFMEMORY;
    HRESULT hr=acquire_window(guest,&d->window);if(FAILED(hr)){HeapFree(GetProcessHeap(),0,d);return hr;}
    d->parent=parent;IDirect3D9_AddRef(parent);d->session=session;
    struct pw_d3d9_device_request q={.operation=PW_D3D9_DEVICE_CREATE,.adapter=adapter,.device_type=type,.behavior_flags=flags};
    struct pw_d3d9_device_reply r={0};if(focus)q.focus_window=d->window->id;
    parameters_to_wire(&q.parameters,parameters,d->window);
    hr=pw_d3d9_session_device(session,parent_ref,&q,&r);
    if(r.operation==q.operation&&!parameters_from_wire(parameters,&r.parameters,d->window)){
        hr=E_FAIL;pw_d3d9_session_cancel(d->session);pw_d3d9_session_join(d->session);
    }
    if(FAILED(hr)){
        if(!release_window(d->window,session)){OutputDebugStringA("PW_D3D9: retaining failed creation cleanup\n");return hr;}
        IDirect3D9_Release(parent);HeapFree(GetProcessHeap(),0,d);return hr;
    }
    d->iface.lpVtbl=&vtable;d->references=1;d->remote=r.object;
    d->creation=(D3DDEVICE_CREATION_PARAMETERS){adapter,type,focus,flags};*out=&d->iface;return hr;
}
