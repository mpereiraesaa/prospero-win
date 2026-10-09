/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_buffer_proxy.h"
#include "pw_d3d9_buffer_client.h"
#include "pw_d3d9_kinds.h"
#include <string.h>
struct buffer {
 union { IDirect3DVertexBuffer9 vb; IDirect3DIndexBuffer9 ib; } iface;
 struct buffer *next;
 IDirect3DDevice9 *parent;
 struct pw_d3d9_object_ref remote;
 ULONG references;
 uint32_t kind;
 LONG busy;
 struct pw_d3d9_buffer_client client;
 struct pw_d3d9_deferred cleanup;
};
static SRWLOCK cache_lock=SRWLOCK_INIT;
static struct buffer *cache;
static struct pw_d3d9_buffer_proxy_ops ops;
static int installed;
static ULONG addref(struct buffer *p)
{
 AcquireSRWLockExclusive(&cache_lock);ULONG n=++p->references;ReleaseSRWLockExclusive(&cache_lock);return n;
}
static uint32_t client_call(void *context,struct pw_d3d9_object_ref ref,const struct pw_d3d9_resource_request *q,struct pw_d3d9_resource_reply *r)
{ return (uint32_t)ops.resource(((struct buffer *)context)->parent,ref,q,r); }
static void client_fail(void *context,uint32_t hr)
{ ops.fail(((struct buffer *)context)->parent,(HRESULT)hr); }
static void free_local(struct buffer *p)
{
 IDirect3DDevice9 *parent=p->parent;
 /* Remote Release destroys the native context and unlocks any live map. */
 p->client.generation=0;
 if(p->client.data)pw_d3d9_buffer_client_cancel(&p->client);
 HeapFree(GetProcessHeap(),0,p);IDirect3DDevice9_Release(parent);
}
static void dispose(void *context)
{
 struct buffer *p=context;HRESULT hr=ops.release(p->parent,p->remote);
 if(hr==RPC_E_CANTCALLOUT_ININPUTSYNCCALL){
  p->cleanup.function=dispose;p->cleanup.context=p;
  /* A failed enqueue cannot justify destroying parent/session from inside an
   * active RPC callback. Retain the shell, mapping and parent on that path. */
  hr=ops.defer(p->parent,&p->cleanup);
  if(FAILED(hr))ops.fail(p->parent,hr);
  return;
 }
 if(FAILED(hr))ops.fail(p->parent,hr);
 free_local(p);
}
static void cleanup(struct buffer *p){dispose(p);}
static ULONG release(struct buffer *p)
{
 struct buffer **link;AcquireSRWLockExclusive(&cache_lock);ULONG n=--p->references;
 if(!n){for(link=&cache;*link&&*link!=p;link=&(*link)->next){}if(*link)*link=p->next;}
 ReleaseSRWLockExclusive(&cache_lock);if(!n)cleanup(p);return n;
}
static HRESULT query(struct buffer *p,REFIID iid,void **out)
{
 if(!out)return E_POINTER;
 *out=NULL;
 if(!IsEqualGUID(iid,&IID_IUnknown)&&!IsEqualGUID(iid,&IID_IDirect3DResource9)&&
    !IsEqualGUID(iid,p->kind==PW_D3D9_KIND_VERTEX_BUFFER?&IID_IDirect3DVertexBuffer9:&IID_IDirect3DIndexBuffer9))return E_NOINTERFACE;
 addref(p);*out=p;return S_OK;
}
static HRESULT device(struct buffer *p,IDirect3DDevice9 **out)
{if(!out)return D3DERR_INVALIDCALL;IDirect3DDevice9_AddRef(p->parent);*out=p->parent;return S_OK;}
static HRESULT begin(struct buffer *p)
{addref(p);if(InterlockedCompareExchange(&p->busy,1,0)){release(p);return RPC_E_CANTCALLOUT_ININPUTSYNCCALL;}return S_OK;}
static void end(struct buffer *p){InterlockedExchange(&p->busy,0);release(p);}
static HRESULT describe(struct buffer *p,struct pw_d3d9_buffer_desc *out)
{
 HRESULT hr=begin(p);if(FAILED(hr))return hr;
 struct pw_d3d9_resource_request q={.operation=PW_D3D9_RESOURCE_DESC};struct pw_d3d9_resource_reply r;
 hr=ops.resource(p->parent,p->remote,&q,&r);
 if(SUCCEEDED(hr)){
  uint32_t type=p->kind==PW_D3D9_KIND_VERTEX_BUFFER?D3DRTYPE_VERTEXBUFFER:D3DRTYPE_INDEXBUFFER;
  if(r.operation!=q.operation||r.desc.type!=type){hr=E_FAIL;ops.fail(p->parent,hr);}else *out=r.desc;
 }
 end(p);return hr;
}
static HRESULT lock_buffer(struct buffer *p,UINT offset,UINT size,void **data,DWORD flags)
{
 if(!data)return D3DERR_INVALIDCALL;
 *data=NULL;HRESULT hr=begin(p);if(FAILED(hr))return hr;
 hr=(HRESULT)pw_d3d9_buffer_client_lock(&p->client,offset,size,flags,data);end(p);return hr;
}
static HRESULT unlock_buffer(struct buffer *p)
{HRESULT hr=begin(p);if(FAILED(hr))return hr;hr=(HRESULT)pw_d3d9_buffer_client_unlock(&p->client);end(p);return hr;}
static DWORD hint(struct buffer *p,uint32_t operation,DWORD priority)
{
 HRESULT hr=begin(p);DWORD result=0;
 if(FAILED(hr)){ops.fail(p->parent,hr);return 0;}
 struct pw_d3d9_resource_request q={.operation=operation,.priority=priority};
 struct pw_d3d9_resource_reply r={0};hr=ops.resource(p->parent,p->remote,&q,&r);
 if(SUCCEEDED(hr)&&(r.operation!=operation||r.hresult!=(uint32_t)hr))hr=E_FAIL;
 if(FAILED(hr))ops.fail(p->parent,hr);else result=r.priority;
 end(p);return result;
}
/* Private data remains unsupported until local COM metadata is implemented. */
#define METHODS(tag,iface) \
static HRESULT WINAPI tag##_query(iface *p,REFIID id,void **o){return query((struct buffer *)p,id,o);} \
static ULONG WINAPI tag##_addref(iface *p){return addref((struct buffer *)p);} \
static ULONG WINAPI tag##_release(iface *p){return release((struct buffer *)p);} \
static HRESULT WINAPI tag##_device(iface *p,IDirect3DDevice9 **o){return device((struct buffer *)p,o);} \
static HRESULT WINAPI tag##_set_private(iface *p,REFGUID g,const void *d,DWORD n,DWORD f){(void)p;(void)g;(void)d;(void)n;(void)f;return D3DERR_NOTAVAILABLE;} \
static HRESULT WINAPI tag##_get_private(iface *p,REFGUID g,void *d,DWORD *n){(void)p;(void)g;(void)d;(void)n;return D3DERR_NOTAVAILABLE;} \
static HRESULT WINAPI tag##_free_private(iface *p,REFGUID g){(void)p;(void)g;return D3DERR_NOTAVAILABLE;} \
static DWORD WINAPI tag##_set_priority(iface *p,DWORD n){return hint((struct buffer *)p,PW_D3D9_RESOURCE_SET_PRIORITY,n);} \
static DWORD WINAPI tag##_get_priority(iface *p){return hint((struct buffer *)p,PW_D3D9_RESOURCE_GET_PRIORITY,0);} \
static void WINAPI tag##_preload(iface *p){(void)hint((struct buffer *)p,PW_D3D9_RESOURCE_PRELOAD,0);} \
static D3DRESOURCETYPE WINAPI tag##_type(iface *p){return ((struct buffer *)p)->kind==PW_D3D9_KIND_VERTEX_BUFFER?D3DRTYPE_VERTEXBUFFER:D3DRTYPE_INDEXBUFFER;} \
static HRESULT WINAPI tag##_lock(iface *p,UINT o,UINT n,void **d,DWORD f){return lock_buffer((struct buffer *)p,o,n,d,f);} \
static HRESULT WINAPI tag##_unlock(iface *p){return unlock_buffer((struct buffer *)p);}
METHODS(vb,IDirect3DVertexBuffer9)
METHODS(ib,IDirect3DIndexBuffer9)
static HRESULT WINAPI vb_desc(IDirect3DVertexBuffer9 *p,D3DVERTEXBUFFER_DESC *out)
{
 if(!out)return D3DERR_INVALIDCALL;
 struct pw_d3d9_buffer_desc d;HRESULT hr=describe((struct buffer *)p,&d);
 if(SUCCEEDED(hr))*out=(D3DVERTEXBUFFER_DESC){d.format,d.type,d.usage,d.pool,d.size,d.fvf};
 return hr;
}
static HRESULT WINAPI ib_desc(IDirect3DIndexBuffer9 *p,D3DINDEXBUFFER_DESC *out)
{
 if(!out)return D3DERR_INVALIDCALL;
 struct pw_d3d9_buffer_desc d;HRESULT hr=describe((struct buffer *)p,&d);
 if(SUCCEEDED(hr))*out=(D3DINDEXBUFFER_DESC){d.format,d.type,d.usage,d.pool,d.size};
 return hr;
}
#define VTABLE(t) {t##_query,t##_addref,t##_release,t##_device,t##_set_private,t##_get_private,t##_free_private,t##_set_priority,t##_get_priority,t##_preload,t##_type,t##_lock,t##_unlock,t##_desc}
static IDirect3DVertexBuffer9Vtbl vb_vtable=VTABLE(vb);
static IDirect3DIndexBuffer9Vtbl ib_vtable=VTABLE(ib);
HRESULT pw_d3d9_buffer_proxy_wrap(IDirect3DDevice9 *parent,uint32_t kind,struct pw_d3d9_object_ref remote,void **out)
{
 if(out)*out=NULL;
 if(!installed||!parent)return E_INVALIDARG;
 IDirect3DDevice9_AddRef(parent);
 struct buffer *p=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*p)),*found=NULL;
 if(!p){ops.fail(parent,E_OUTOFMEMORY);IDirect3DDevice9_Release(parent);return E_OUTOFMEMORY;}
 p->parent=parent;p->kind=kind;p->remote=remote;p->references=1;
 if(!out||!remote.id||!remote.generation||(kind!=PW_D3D9_KIND_VERTEX_BUFFER&&kind!=PW_D3D9_KIND_INDEX_BUFFER)){
  cleanup(p);return E_INVALIDARG;
 }
 if(kind==PW_D3D9_KIND_VERTEX_BUFFER)p->iface.vb.lpVtbl=&vb_vtable;else p->iface.ib.lpVtbl=&ib_vtable;
 p->client=(struct pw_d3d9_buffer_client){.context=p,.call=client_call,.fail=client_fail,.object=remote};
 AcquireSRWLockExclusive(&cache_lock);
 for(found=cache;found;found=found->next)if(found->parent==parent&&found->kind==kind&&found->remote.id==remote.id&&found->remote.generation==remote.generation){found->references++;break;}
 if(!found){p->next=cache;cache=p;}
 ReleaseSRWLockExclusive(&cache_lock);
 if(found){cleanup(p);p=found;}*out=p;return S_OK;
}
HRESULT pw_d3d9_buffer_proxy_resolve(IDirect3DDevice9 *parent,IUnknown *unknown,uint32_t kind,struct pw_d3d9_object_ref *out)
{
 if(!out)return E_POINTER;
 *out=(struct pw_d3d9_object_ref){0};if(!unknown)return S_OK;
 HRESULT hr=D3DERR_INVALIDCALL;AcquireSRWLockShared(&cache_lock);
 for(struct buffer *p=cache;p;p=p->next)if((void *)p==(void *)unknown&&p->parent==parent&&p->kind==kind){*out=p->remote;hr=S_OK;break;}
 ReleaseSRWLockShared(&cache_lock);return hr;
}
static HRESULT create(IDirect3DDevice9 *parent,uint32_t kind,UINT length,DWORD usage,DWORD format,D3DPOOL pool,void **out,HANDLE *shared)
{
 if(!out)return D3DERR_INVALIDCALL;
 *out=NULL;if(shared)return D3DERR_NOTAVAILABLE;
 struct pw_d3d9_resource_request q={.operation=kind==PW_D3D9_KIND_VERTEX_BUFFER?PW_D3D9_RESOURCE_CREATE_VB:PW_D3D9_RESOURCE_CREATE_IB,.length=length,.usage=usage,.format_fvf=format,.pool=pool};
 struct pw_d3d9_resource_reply r;IDirect3DDevice9_AddRef(parent);
 HRESULT hr=ops.resource(parent,(struct pw_d3d9_object_ref){0},&q,&r);
 if(SUCCEEDED(hr)){
  if(r.operation!=q.operation||!r.object.id||!r.object.generation){hr=E_FAIL;ops.fail(parent,hr);}
  else hr=pw_d3d9_buffer_proxy_wrap(parent,kind,r.object,out);
 }
 IDirect3DDevice9_Release(parent);return hr;
}
static HRESULT WINAPI create_vb(IDirect3DDevice9 *p,UINT n,DWORD u,DWORD f,D3DPOOL pool,IDirect3DVertexBuffer9 **o,HANDLE *s)
{return create(p,PW_D3D9_KIND_VERTEX_BUFFER,n,u,f,pool,(void **)o,s);}
static HRESULT WINAPI create_ib(IDirect3DDevice9 *p,UINT n,DWORD u,D3DFORMAT f,D3DPOOL pool,IDirect3DIndexBuffer9 **o,HANDLE *s)
{return create(p,PW_D3D9_KIND_INDEX_BUFFER,n,u,f,pool,(void **)o,s);}
HRESULT pw_d3d9_buffer_proxy_install(IDirect3DDevice9Vtbl *vtable,const struct pw_d3d9_buffer_proxy_ops *callbacks)
{
 if(!vtable||!callbacks||!callbacks->resource||!callbacks->release||!callbacks->defer||!callbacks->fail)return E_INVALIDARG;
 AcquireSRWLockExclusive(&cache_lock);
 if(installed&&(ops.resource!=callbacks->resource||ops.release!=callbacks->release||ops.defer!=callbacks->defer||ops.fail!=callbacks->fail)){
  ReleaseSRWLockExclusive(&cache_lock);return E_INVALIDARG;
 }
 ops=*callbacks;installed=1;ReleaseSRWLockExclusive(&cache_lock);
 vtable->CreateVertexBuffer=create_vb;vtable->CreateIndexBuffer=create_ib;return S_OK;
}
