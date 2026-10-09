/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_query_proxy.h"
#include <string.h>
struct proxy {IDirect3DQuery9 iface;LONG refs;IDirect3DDevice9 *parent;struct pw_d3d9_object_ref remote;uint32_t type,size;struct pw_d3d9_deferred cleanup;};
static struct pw_d3d9_query_proxy_ops ops;
static IDirect3DQuery9Vtbl vtable;
static struct proxy *impl(IDirect3DQuery9 *p){return (struct proxy *)p;}
static ULONG WINAPI addref(IDirect3DQuery9 *p){return InterlockedIncrement(&impl(p)->refs);}
static void free_local(struct proxy *p){IDirect3DDevice9 *parent=p->parent;HeapFree(GetProcessHeap(),0,p);IDirect3DDevice9_Release(parent);}
static void finish(void *context)
{
 struct proxy *p=context;HRESULT hr=ops.release(p->parent,p->remote);
 if(hr==RPC_E_CANTCALLOUT_ININPUTSYNCCALL){p->cleanup.function=finish;p->cleanup.context=p;hr=ops.defer(p->parent,&p->cleanup);if(FAILED(hr))ops.fail(p->parent,hr);return;}
 if(FAILED(hr))ops.fail(p->parent,hr);
 free_local(p);
}
static ULONG WINAPI release(IDirect3DQuery9 *p){LONG n=InterlockedDecrement(&impl(p)->refs);if(!n)finish(impl(p));return n;}
static HRESULT WINAPI query(IDirect3DQuery9 *p,REFIID iid,void **out)
{if(!out)return E_POINTER;*out=NULL;if(!iid || (!IsEqualGUID(iid,&IID_IUnknown)&&!IsEqualGUID(iid,&IID_IDirect3DQuery9)))return E_NOINTERFACE;addref(p);*out=p;return S_OK;}
static HRESULT WINAPI device(IDirect3DQuery9 *p,IDirect3DDevice9 **out)
{if(!out)return D3DERR_INVALIDCALL;*out=impl(p)->parent;IDirect3DDevice9_AddRef(*out);return S_OK;}
static D3DQUERYTYPE WINAPI type(IDirect3DQuery9 *p){return impl(p)->type;}
static DWORD WINAPI size(IDirect3DQuery9 *p){return impl(p)->size;}
static HRESULT invoke(IDirect3DDevice9 *parent,struct pw_d3d9_object_ref ref,const struct pw_d3d9_query_request *q,struct pw_d3d9_query_reply *r,int *reply_valid)
{
 unsigned char bytes[PW_D3D9_QUERY_WIRE_MAX];size_t n;HRESULT hr=ops.call(parent,ref,q,r);*reply_valid=0;
 if(!r->method && FAILED(hr))return hr; /* transport failure, no backend bytes */
 if(r->hresult!=(uint32_t)hr || pw_d3d9_query_reply_encode(bytes,sizeof(bytes),&n,q,r)){ops.fail(parent,E_FAIL);return E_FAIL;}
 *reply_valid=1;return hr;
}
static HRESULT WINAPI issue(IDirect3DQuery9 *iface,DWORD flags)
{
 struct proxy *p=impl(iface);struct pw_d3d9_query_request q={.method=PW_D3D9_QUERY_ISSUE,.flags=flags};struct pw_d3d9_query_reply r={0};HRESULT hr;int valid;
 addref(iface);hr=invoke(p->parent,p->remote,&q,&r,&valid);release(iface);return hr;
}
static HRESULT WINAPI data(IDirect3DQuery9 *iface,void *out,DWORD count,DWORD flags)
{
 struct proxy *p=impl(iface);struct pw_d3d9_query_request q={.method=PW_D3D9_QUERY_DATA,.size=count,.flags=flags,.has_data=out!=NULL};struct pw_d3d9_query_reply r={0};HRESULT hr;int valid;
 if(count>p->size)return D3DERR_INVALIDCALL;
 addref(iface);if(out && count)memcpy(q.data,out,count);
 hr=invoke(p->parent,p->remote,&q,&r,&valid);if(valid && out && count)memcpy(out,r.data,count);release(iface);return hr;
}
static IDirect3DQuery9Vtbl vtable={query,addref,release,device,type,size,issue,data};
static HRESULT WINAPI create(IDirect3DDevice9 *parent,D3DQUERYTYPE type,IDirect3DQuery9 **out)
{
 struct proxy *p=NULL;struct pw_d3d9_query_request q={.method=PW_D3D9_QUERY_CREATE,.type=type,.want_object=out!=NULL};struct pw_d3d9_query_reply r={0};HRESULT hr;int valid;
 if(out)*out=NULL;
 IDirect3DDevice9_AddRef(parent);
 if(out){p=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*p));if(!p){IDirect3DDevice9_Release(parent);return E_OUTOFMEMORY;}p->iface.lpVtbl=&vtable;p->refs=1;p->parent=parent;}
 hr=invoke(parent,(struct pw_d3d9_object_ref){0},&q,&r,&valid);
 if(FAILED(hr)){if(p)free_local(p);else IDirect3DDevice9_Release(parent);return hr;}
 if(p){p->remote=r.object;p->type=r.type;p->size=r.size;*out=&p->iface;}else IDirect3DDevice9_Release(parent);
 return hr;
}
void pw_d3d9_query_proxy_install(IDirect3DDevice9Vtbl *table,const struct pw_d3d9_query_proxy_ops *callbacks){ops=*callbacks;table->CreateQuery=create;}
