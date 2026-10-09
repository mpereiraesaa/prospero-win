/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_stateblock_client.h"
struct proxy {
 IDirect3DStateBlock9 iface;ULONG refs;IDirect3DDevice9 *parent;
 struct pw_d3d9_object_ref remote;struct pw_d3d9_deferred cleanup;struct proxy *next;
};
static SRWLOCK cache_lock=SRWLOCK_INIT;
static struct proxy *cache;
static struct pw_d3d9_stateblock_client_ops ops;
static IDirect3DStateBlock9Vtbl vtable;
static struct proxy *impl(IDirect3DStateBlock9 *iface){return (struct proxy *)iface;}
static ULONG WINAPI addref(IDirect3DStateBlock9 *iface)
{struct proxy *p=impl(iface);ULONG refs;AcquireSRWLockExclusive(&cache_lock);refs=++p->refs;ReleaseSRWLockExclusive(&cache_lock);return refs;}
static void destroy_local(struct proxy *p)
{IDirect3DDevice9 *parent=p->parent;HeapFree(GetProcessHeap(),0,p);IDirect3DDevice9_Release(parent);}
static void finish(void *context)
{
 struct proxy *p=context;HRESULT hr=ops.release(p->parent,p->remote);
 if(hr==RPC_E_CANTCALLOUT_ININPUTSYNCCALL){
  p->cleanup.function=finish;p->cleanup.context=p;
  hr=ops.defer(p->parent,&p->cleanup);
  if(FAILED(hr))ops.fail(p->parent,hr); /* Retain ownership if enqueue failed. */
  return;
 }
 if(FAILED(hr))ops.fail(p->parent,hr);
 destroy_local(p);
}
static ULONG WINAPI release(IDirect3DStateBlock9 *iface)
{
 struct proxy *p=impl(iface);ULONG refs;AcquireSRWLockExclusive(&cache_lock);refs=--p->refs;
 if(!refs){struct proxy **link=&cache;while(*link && *link!=p)link=&(*link)->next;if(*link)*link=p->next;}
 ReleaseSRWLockExclusive(&cache_lock);if(!refs)finish(p);return refs;
}
static HRESULT WINAPI query(IDirect3DStateBlock9 *iface,REFIID iid,void **out)
{
 if(!out)return E_POINTER;
 *out=NULL;
 if(!iid || (!IsEqualGUID(iid,&IID_IUnknown) && !IsEqualGUID(iid,&IID_IDirect3DStateBlock9)))return E_NOINTERFACE;
 addref(iface);*out=iface;return S_OK;
}
static HRESULT WINAPI get_device(IDirect3DStateBlock9 *iface,IDirect3DDevice9 **out)
{if(!out)return D3DERR_INVALIDCALL;*out=impl(iface)->parent;IDirect3DDevice9_AddRef(*out);return S_OK;}
static HRESULT invoke(IDirect3DDevice9 *parent,struct pw_d3d9_object_ref ref,const struct pw_d3d9_stateblock_request *q,struct pw_d3d9_stateblock_reply *r)
{
 unsigned char wire[16];HRESULT hr=ops.call(parent,ref,q,r);
 if(FAILED(hr))return hr;
 if(r->hresult!=(uint32_t)hr || pw_d3d9_stateblock_reply_encode(wire,sizeof(wire),q,r)){
  ops.fail(parent,E_FAIL);return E_FAIL;
 }
 return hr;
}
static HRESULT block_call(IDirect3DStateBlock9 *iface,uint32_t method)
{
 struct proxy *p=impl(iface);struct pw_d3d9_stateblock_request q={method,0};struct pw_d3d9_stateblock_reply r={0};HRESULT hr;
 addref(iface);hr=invoke(p->parent,p->remote,&q,&r);release(iface);return hr;
}
static HRESULT WINAPI capture(IDirect3DStateBlock9 *iface){return block_call(iface,PW_D3D9_SB_CAPTURE);}
static HRESULT WINAPI apply(IDirect3DStateBlock9 *iface){return block_call(iface,PW_D3D9_SB_APPLY);}
static IDirect3DStateBlock9Vtbl vtable={query,addref,release,get_device,capture,apply};
static HRESULT create_block(IDirect3DDevice9 *parent,uint32_t method,D3DSTATEBLOCKTYPE type,IDirect3DStateBlock9 **out)
{
 struct pw_d3d9_stateblock_request q={method,(uint32_t)type};struct pw_d3d9_stateblock_reply r={0};struct proxy *p,*found=NULL;HRESULT hr;
 if(!out)return D3DERR_INVALIDCALL;
 *out=NULL;
 /* Allocate before RPC so OOM cannot strand an acquired remote reference. */
 p=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*p));if(!p)return E_OUTOFMEMORY;
 p->iface.lpVtbl=&vtable;p->refs=1;p->parent=parent;IDirect3DDevice9_AddRef(parent);
 hr=invoke(parent,(struct pw_d3d9_object_ref){0},&q,&r);
 if(FAILED(hr)){destroy_local(p);return hr;}
 p->remote=r.object;
 AcquireSRWLockExclusive(&cache_lock);
 for(struct proxy *it=cache;it;it=it->next)if(it->parent==parent && it->remote.id==r.object.id && it->remote.generation==r.object.generation){found=it;found->refs++;break;}
 if(!found){p->next=cache;cache=p;}
 ReleaseSRWLockExclusive(&cache_lock);
 if(found){*out=&found->iface;finish(p);}else *out=&p->iface;
 return hr;
}
static HRESULT WINAPI create(IDirect3DDevice9 *parent,D3DSTATEBLOCKTYPE type,IDirect3DStateBlock9 **out)
{return create_block(parent,PW_D3D9_SB_CREATE,type,out);}
static HRESULT WINAPI begin(IDirect3DDevice9 *parent)
{
 struct pw_d3d9_stateblock_request q={PW_D3D9_SB_BEGIN,0};struct pw_d3d9_stateblock_reply r={0};HRESULT hr;
 IDirect3DDevice9_AddRef(parent);hr=invoke(parent,(struct pw_d3d9_object_ref){0},&q,&r);IDirect3DDevice9_Release(parent);return hr;
}
static HRESULT WINAPI end(IDirect3DDevice9 *parent,IDirect3DStateBlock9 **out)
{return create_block(parent,PW_D3D9_SB_END,0,out);}
void pw_d3d9_stateblock_client_install(IDirect3DDevice9Vtbl *table,const struct pw_d3d9_stateblock_client_ops *callbacks)
{ops=*callbacks;table->CreateStateBlock=create;table->BeginStateBlock=begin;table->EndStateBlock=end;}
