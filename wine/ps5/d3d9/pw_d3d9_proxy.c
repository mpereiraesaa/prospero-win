/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_session.h"
#include <d3d9.h>
#include <string.h>

struct proxy {
    IDirect3D9 iface;
    ULONG references;
    struct pw_d3d9_object_ref remote;
    struct proxy *next;
};
static SRWLOCK lock=SRWLOCK_INIT;
static struct pw_d3d9_session *session;
static struct proxy *proxies;
static struct proxy *object(IDirect3D9 *iface) {return (struct proxy *)iface;}
static HRESULT call(IDirect3D9 *iface,struct pw_d3d9_factory_request *q,struct pw_d3d9_factory_reply *r)
{return pw_d3d9_session_factory(session,object(iface)->remote,q,r);}
static ULONG WINAPI proxy_addref(IDirect3D9 *iface)
{
    AcquireSRWLockExclusive(&lock);
    ULONG refs=++object(iface)->references;
    ReleaseSRWLockExclusive(&lock);return refs;
}
static HRESULT WINAPI proxy_query(IDirect3D9 *iface,REFIID iid,void **out)
{
    if(!out)return E_POINTER;
    *out=NULL;
    if(!IsEqualGUID(iid,&IID_IUnknown)&&!IsEqualGUID(iid,&IID_IDirect3D9))return E_NOINTERFACE;
    proxy_addref(iface);*out=iface;return S_OK;
}
static ULONG WINAPI proxy_release(IDirect3D9 *iface)
{
    struct proxy *p=object(iface),**link;
    AcquireSRWLockExclusive(&lock);
    ULONG refs=--p->references;
    if(!refs){
        for(link=&proxies;*link!=p;link=&(*link)->next){}
        *link=p->next;
        pw_d3d9_session_release(session,p->remote);
        HeapFree(GetProcessHeap(),0,p);
        if(!proxies){pw_d3d9_session_close(session);session=NULL;}
    }
    ReleaseSRWLockExclusive(&lock);return refs;
}
static HRESULT WINAPI proxy_software(IDirect3D9 *iface,void *callback)
{(void)iface;(void)callback;return D3DERR_NOTAVAILABLE;}
static UINT WINAPI proxy_count(IDirect3D9 *iface)
{
    struct pw_d3d9_factory_request q={.method=4};struct pw_d3d9_factory_reply r;
    return SUCCEEDED(call(iface,&q,&r))?r.count:0;
}
static HRESULT WINAPI proxy_identifier(IDirect3D9 *iface,UINT adapter,DWORD flags,D3DADAPTER_IDENTIFIER9 *out)
{
    struct pw_d3d9_factory_request q={.method=5,.adapter=adapter,.flags=flags};struct pw_d3d9_factory_reply r;
    if(!out)return D3DERR_INVALIDCALL;
    HRESULT hr=call(iface,&q,&r);if(FAILED(hr))return hr;
    D3DADAPTER_IDENTIFIER9 value={0};
    memcpy(value.Driver,r.identifier.driver,sizeof(value.Driver));
    memcpy(value.Description,r.identifier.description,sizeof(value.Description));
    memcpy(value.DeviceName,r.identifier.device_name,sizeof(value.DeviceName));
    value.DriverVersion.QuadPart=(LONGLONG)r.identifier.driver_version;
    value.VendorId=r.identifier.vendor_id;value.DeviceId=r.identifier.device_id;
    value.SubSysId=r.identifier.subsystem_id;value.Revision=r.identifier.revision;
    value.DeviceIdentifier.Data1=r.identifier.guid_data1;value.DeviceIdentifier.Data2=r.identifier.guid_data2;
    value.DeviceIdentifier.Data3=r.identifier.guid_data3;memcpy(value.DeviceIdentifier.Data4,r.identifier.guid_data4,8);
    value.WHQLLevel=r.identifier.whql_level;*out=value;return hr;
}
static UINT WINAPI proxy_mode_count(IDirect3D9 *iface,UINT adapter,D3DFORMAT format)
{
    struct pw_d3d9_factory_request q={.method=6,.adapter=adapter,.format=format};struct pw_d3d9_factory_reply r;
    return SUCCEEDED(call(iface,&q,&r))?r.count:0;
}
static HRESULT copy_mode(IDirect3D9 *iface,struct pw_d3d9_factory_request *q,D3DDISPLAYMODE *out)
{
    struct pw_d3d9_factory_reply r;
    if(!out)return D3DERR_INVALIDCALL;
    HRESULT hr=call(iface,q,&r);
    if(SUCCEEDED(hr))*out=(D3DDISPLAYMODE){r.mode.width,r.mode.height,r.mode.refresh_rate,r.mode.format};
    return hr;
}
static HRESULT WINAPI proxy_enum(IDirect3D9 *iface,UINT adapter,D3DFORMAT format,UINT mode,D3DDISPLAYMODE *out)
{struct pw_d3d9_factory_request q={.method=7,.adapter=adapter,.format=format,.mode=mode};return copy_mode(iface,&q,out);}
static HRESULT WINAPI proxy_mode(IDirect3D9 *iface,UINT adapter,D3DDISPLAYMODE *out)
{struct pw_d3d9_factory_request q={.method=8,.adapter=adapter};return copy_mode(iface,&q,out);}
static HRESULT WINAPI proxy_type(IDirect3D9 *iface,UINT adapter,D3DDEVTYPE type,D3DFORMAT display,D3DFORMAT back,BOOL windowed)
{
    struct pw_d3d9_factory_request q={.method=9,.adapter=adapter,.device_type=type,.format=display,.format2=back,.windowed=!!windowed};
    struct pw_d3d9_factory_reply r;return call(iface,&q,&r);
}
static HRESULT WINAPI proxy_format(IDirect3D9 *iface,UINT adapter,D3DDEVTYPE type,D3DFORMAT display,DWORD usage,D3DRESOURCETYPE resource,D3DFORMAT check)
{
    struct pw_d3d9_factory_request q={.method=10,.adapter=adapter,.device_type=type,.format=display,.format2=check,.usage=usage,.resource_type=resource};
    struct pw_d3d9_factory_reply r;return call(iface,&q,&r);
}
static HRESULT WINAPI proxy_multisample(IDirect3D9 *iface,UINT adapter,D3DDEVTYPE type,D3DFORMAT format,BOOL windowed,D3DMULTISAMPLE_TYPE samples,DWORD *quality)
{
    struct pw_d3d9_factory_request q={.method=11,.adapter=adapter,.device_type=type,.format=format,.windowed=!!windowed,.multisample=samples};
    struct pw_d3d9_factory_reply r;HRESULT hr=call(iface,&q,&r);
    if(SUCCEEDED(hr)&&quality)*quality=r.count;
    return hr;
}
static HRESULT WINAPI proxy_depth(IDirect3D9 *iface,UINT adapter,D3DDEVTYPE type,D3DFORMAT display,D3DFORMAT render,D3DFORMAT depth)
{
    struct pw_d3d9_factory_request q={.method=12,.adapter=adapter,.device_type=type,.format=display,.format2=render,.format3=depth};
    struct pw_d3d9_factory_reply r;return call(iface,&q,&r);
}
static HRESULT WINAPI proxy_conversion(IDirect3D9 *iface,UINT adapter,D3DDEVTYPE type,D3DFORMAT source,D3DFORMAT target)
{
    struct pw_d3d9_factory_request q={.method=13,.adapter=adapter,.device_type=type,.format=source,.format2=target};
    struct pw_d3d9_factory_reply r;return call(iface,&q,&r);
}
static HRESULT WINAPI proxy_caps(IDirect3D9 *iface,UINT adapter,D3DDEVTYPE type,D3DCAPS9 *out)
{
    struct pw_d3d9_factory_request q={.method=14,.adapter=adapter,.device_type=type};struct pw_d3d9_factory_reply r;
    if(!out)return D3DERR_INVALIDCALL;
    HRESULT hr=call(iface,&q,&r);if(FAILED(hr))return hr;
    D3DCAPS9 value={0};
#define COPY_CAP(kind,name,native) _Static_assert(sizeof(value.native)==4,"caps member width");memcpy(&value.native,&r.caps.name,4);
    PW_D3D9_CAP_FIELDS(COPY_CAP)
#undef COPY_CAP
    *out=value;return hr;
}
static HMONITOR WINAPI proxy_monitor(IDirect3D9 *iface,UINT adapter)
{(void)iface;(void)adapter;return NULL;}
static HRESULT WINAPI proxy_device(IDirect3D9 *iface,UINT adapter,D3DDEVTYPE type,HWND focus,DWORD flags,D3DPRESENT_PARAMETERS *parameters,IDirect3DDevice9 **out)
{
    (void)iface;(void)adapter;(void)type;(void)focus;(void)flags;(void)parameters;
    if(out)*out=NULL;
    return D3DERR_NOTAVAILABLE;
}
static IDirect3D9Vtbl vtable={proxy_query,proxy_addref,proxy_release,proxy_software,
    proxy_count,proxy_identifier,proxy_mode_count,proxy_enum,proxy_mode,proxy_type,
    proxy_format,proxy_multisample,proxy_depth,proxy_conversion,proxy_caps,proxy_monitor,proxy_device};
__declspec(dllexport) IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk)
{
    struct proxy *p=NULL;struct pw_d3d9_object_ref remote;
    AcquireSRWLockExclusive(&lock);
    if(!session){
        WCHAR service[260],backend[260];
        DWORD a=GetEnvironmentVariableW(L"PW_D3D9_SERVICE64",service,260);
        DWORD b=GetEnvironmentVariableW(L"PW_D3D9_BACKEND64",backend,260);
        if(!a||a>=260||!b||b>=260||FAILED(pw_d3d9_session_open(service,backend,&session)))goto done;
    }
    if(FAILED(pw_d3d9_session_create(session,sdk,&remote)))goto cleanup;
    for(p=proxies;p;p=p->next)if(p->remote.id==remote.id&&p->remote.generation==remote.generation){
        p->references++;pw_d3d9_session_release(session,remote);goto done;
    }
    p=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*p));
    if(!p){pw_d3d9_session_release(session,remote);goto cleanup;}
    p->iface.lpVtbl=&vtable;p->references=1;p->remote=remote;p->next=proxies;proxies=p;
    goto done;
 cleanup:
    if(!proxies){pw_d3d9_session_close(session);session=NULL;}
 done:
    ReleaseSRWLockExclusive(&lock);return p?&p->iface:NULL;
}
__declspec(dllexport) HRESULT WINAPI Direct3DCreate9Ex(UINT sdk,IDirect3D9Ex **out)
{(void)sdk;if(!out)return E_POINTER;*out=NULL;return D3DERR_NOTAVAILABLE;}
