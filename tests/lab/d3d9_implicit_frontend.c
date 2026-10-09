/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <windows.h>
static FARPROC WINAPI fixture_get_proc(HMODULE,LPCSTR);
#define GetProcAddress fixture_get_proc
#define PW_D3D9_ENABLE_DEVICE
#define PW_D3D9_ENABLE_TEXTURE
#define PW_D3D9_ENABLE_IMPLICIT
#include "../../wine/ps5/d3d9/pw_d3d9_device_proxy.c"
#undef GetProcAddress
#include <assert.h>
static unsigned events[64],event_count,cancels,joins,remote_resets,owner_installs;
static unsigned prepared,keep_old,in_callback,defer_failure,owner_count;
static HRESULT backend_hr,prepare_hr,finish_hr;
static unsigned disposition,missing_reset,missing_prepare,missing_finish;
static struct pw_d3d9_deferred *deferred;
static LONG parent_refs=1;
static ULONG WINAPI parent_add(IDirect3D9 *p){(void)p;return InterlockedIncrement(&parent_refs);}
static ULONG WINAPI parent_drop(IDirect3D9 *p){(void)p;return InterlockedDecrement(&parent_refs);}
static IDirect3D9Vtbl parent_table={.AddRef=parent_add,.Release=parent_drop};
static IDirect3D9 factory={&parent_table};
static void event(unsigned value){assert(event_count<64);events[event_count++]=value;}
static ULONG_PTR WINAPI fake_driver(ULONG_PTR hwnd,ULONG_PTR pointer,DWORD method)
{
 (void)hwnd;assert(method==PW_D3D9_GUEST_WINDOW_CALL);
 struct pw_d3d9_guest_window_request *q=(void *)pointer;
 if(q->operation==PW_D3D9_GUEST_REGISTER)q->id=(struct pw_d3d9_window_id){1,2,3};
 return PW_D3D9_WINDOW_OK;
}
static FARPROC WINAPI fixture_get_proc(HMODULE module,LPCSTR name)
{if(!strcmp(name,"NtUserCallTwoParam"))return (FARPROC)fake_driver;return GetProcAddress(module,name);}
int pw_d3d9_session_in_callback(void){return in_callback;}
void pw_d3d9_session_cancel(struct pw_d3d9_session *s){(void)s;cancels++;}
HRESULT pw_d3d9_session_join(struct pw_d3d9_session *s){(void)s;joins++;return S_OK;}
HRESULT pw_d3d9_session_defer(struct pw_d3d9_session *s,struct pw_d3d9_deferred *d)
{(void)s;event(90);if(defer_failure)return E_FAIL;assert(!deferred);deferred=d;return S_OK;}
HRESULT pw_d3d9_session_release(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref)
{(void)s;assert(!in_callback);event(ref.id==50?80:70);return S_OK;}
HRESULT pw_d3d9_session_texture(struct pw_d3d9_session *s,struct pw_d3d9_object_ref o,const struct pw_d3d9_texture_request *q,struct pw_d3d9_texture_reply *r)
{(void)s;(void)o;(void)q;(void)r;return D3DERR_NOTAVAILABLE;}
HRESULT pw_d3d9_session_device(struct pw_d3d9_session *s,struct pw_d3d9_object_ref o,const struct pw_d3d9_device_request *q,struct pw_d3d9_device_reply *r)
{
 (void)s;(void)o;assert(!in_callback);
 if(q->operation==PW_D3D9_DEVICE_RESET){assert(prepared);remote_resets++;event(30);if(missing_reset)return E_FAIL;}
 *r=(struct pw_d3d9_device_reply){.operation=q->operation,.hresult=(uint32_t)backend_hr,.parameters=q->parameters,.object={50,1}};
 return backend_hr;
}
HRESULT pw_d3d9_session_implicit(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref,const struct pw_d3d9_implicit_request *q,struct pw_d3d9_implicit_reply *r)
{
 (void)s;assert(ref.id==50&&!in_callback);event(10*q->operation);
 if((q->operation==PW_D3D9_IMPLICIT_PREPARE&&missing_prepare)||(q->operation==PW_D3D9_IMPLICIT_FINISH&&missing_finish))return E_FAIL;
 HRESULT hr=q->operation==PW_D3D9_IMPLICIT_PREPARE?prepare_hr:q->operation==PW_D3D9_IMPLICIT_FINISH?finish_hr:S_OK;
 *r=(struct pw_d3d9_implicit_reply){.operation=q->operation,.hresult=(uint32_t)hr};
 if(SUCCEEDED(hr)&&(q->operation==PW_D3D9_IMPLICIT_LIST||q->operation==PW_D3D9_IMPLICIT_FINISH)){
  r->count=2;r->objects[0]=(struct pw_d3d9_object_ref){60,1};r->objects[1]=(struct pw_d3d9_object_ref){61,1};
 }
 if(SUCCEEDED(hr)&&q->operation==PW_D3D9_IMPLICIT_FINISH)r->disposition=disposition;
 return hr;
}
void pw_d3d9_texture_proxy_install(IDirect3DDevice9Vtbl *v,const struct pw_d3d9_texture_proxy_ops *o){(void)v;(void)o;}
HRESULT pw_d3d9_texture_proxy_owners_install(IDirect3DDevice9 *d,const struct pw_d3d9_object_ref *r,UINT n)
{(void)d;assert(n==2&&r[0].id==60&&r[1].id==61&&!prepared);owner_count=n;owner_installs++;event(50);return S_OK;}
HRESULT pw_d3d9_texture_proxy_owners_prepare_reset(IDirect3DDevice9 *d,struct pw_d3d9_object_ref *r,UINT cap,UINT *n)
{(void)d;assert(!prepared&&owner_count==2&&cap==16);prepared=1;*n=1;r[0]=(struct pw_d3d9_object_ref){60,1};event(15);return S_OK;}
void pw_d3d9_texture_proxy_owners_finish_reset(IDirect3DDevice9 *d,BOOL keep)
{(void)d;assert(prepared);prepared=0;keep_old=keep;if(!keep)owner_count=0;event(keep?41:42);}
ULONG pw_d3d9_texture_proxy_parent_release(IDirect3DDevice9 *d,LONG *r)
{(void)d;LONG n=InterlockedDecrement(r);if(!n)InterlockedExchange(r,1);return n;}
HRESULT pw_d3d9_texture_proxy_owners_dispose(IDirect3DDevice9 *iface)
{assert(!in_callback);event(60);if(owner_count){assert(SUCCEEDED(release_object(iface,(struct pw_d3d9_object_ref){60,1})));assert(device(iface)->references==1);}owner_count=0;return S_OK;}
static HWND make_window(void){return CreateWindowW(L"STATIC",L"implicit owner fixture",WS_POPUP,0,0,640,480,NULL,NULL,GetModuleHandleW(NULL),NULL);}
static IDirect3DDevice9 *create(HWND hwnd)
{
 backend_hr=prepare_hr=finish_hr=S_OK;disposition=PW_D3D9_IMPLICIT_RETIRED;missing_reset=missing_prepare=missing_finish=0;
 D3DPRESENT_PARAMETERS p={.Windowed=TRUE,.hDeviceWindow=hwnd};IDirect3DDevice9 *d=NULL;
 assert(pw_d3d9_device_proxy_create(&factory,(void *)1,(struct pw_d3d9_object_ref){1,1},0,D3DDEVTYPE_HAL,hwnd,0,&p,&d)==S_OK);
 assert(d&&owner_count==2&&parent_refs==2);return d;
}
int main(void)
{
 HWND hwnd=make_window();assert(hwnd);IDirect3DDevice9 *d=create(hwnd);D3DPRESENT_PARAMETERS p={.Windowed=TRUE,.hDeviceWindow=hwnd};
 unsigned calls=remote_resets,installs=owner_installs;
 prepare_hr=D3DERR_INVALIDCALL;assert(reset(d,&p)==prepare_hr&&keep_old&&!prepared&&remote_resets==calls&&!device(d)->failed);
 prepare_hr=S_OK;backend_hr=D3DERR_INVALIDCALL;disposition=PW_D3D9_IMPLICIT_RESTORED;
 assert(reset(d,&p)==backend_hr&&keep_old&&owner_installs==installs);
 disposition=PW_D3D9_IMPLICIT_RETIRED;assert(reset(d,&p)==backend_hr&&!keep_old&&owner_installs==++installs);
 backend_hr=S_OK;assert(reset(d,&p)==S_OK&&owner_installs==++installs);
 in_callback=1;calls=remote_resets;assert(reset(d,&p)==RPC_E_CANTCALLOUT_ININPUTSYNCCALL&&remote_resets==calls&&!prepared);in_callback=0;
 missing_reset=1;assert(reset(d,&p)==E_FAIL&&keep_old&&device(d)->failed&&!prepared);missing_reset=0;
 event_count=0;in_callback=1;assert(release(d)==0&&deferred&&event_count==1&&events[0]==90);in_callback=0;
 struct pw_d3d9_deferred *node=deferred;deferred=NULL;node->function(node->context);
 assert(parent_refs==1&&event_count==5&&events[1]==60&&events[2]==70&&events[3]==40&&events[4]==80);
 d=create(hwnd);missing_prepare=1;assert(reset(d,&p)==E_FAIL&&device(d)->failed&&keep_old&&!prepared);assert(release(d)==0&&parent_refs==1);
 d=create(hwnd);finish_hr=E_FAIL;backend_hr=D3DERR_DEVICELOST;assert(reset(d,&p)==D3DERR_DEVICELOST&&device(d)->failed&&keep_old);assert(release(d)==0&&parent_refs==1);
 d=create(hwnd);missing_finish=1;assert(reset(d,&p)==E_FAIL&&device(d)->failed&&keep_old);assert(release(d)==0&&parent_refs==1);
 d=create(hwnd);event_count=0;in_callback=1;defer_failure=1;assert(release(d)==0&&device(d)->failed&&device(d)->references==1&&event_count==1);in_callback=0;defer_failure=0;
 cleanup_device(device(d));assert(parent_refs==1);
 assert(DestroyWindow(hwnd));pw_d3d9_device_proxy_detach();assert(!windows);
 printf("PW_IMPLICIT_FRONTEND PASS installs=%u resets=%u cancels=%u joins=%u\n",owner_installs,remote_resets,cancels,joins);return 0;
}
