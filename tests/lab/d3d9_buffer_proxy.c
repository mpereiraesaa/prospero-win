/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "../../wine/ps5/d3d9/pw_d3d9_buffer_proxy.h"
#include "../../wine/ps5/d3d9/pw_d3d9_buffer_client.h"
#include "../../wine/ps5/d3d9/pw_d3d9_kinds.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %u: %s\n",__LINE__,#x);return 1;}}while(0)
static ULONG parent_refs=1;
static unsigned releases,failed,blocked,next_id,release_in_call,defer_failure;
static struct pw_d3d9_deferred *rejected;
static IDirect3DVertexBuffer9 *reentrant;
static struct pw_d3d9_deferred *pending;
static unsigned char bytes[16384];
static struct pw_d3d9_buffer_desc desc[8];
static ULONG WINAPI parent_add(IDirect3DDevice9 *p){(void)p;return ++parent_refs;}
static ULONG WINAPI parent_drop(IDirect3DDevice9 *p){(void)p;return --parent_refs;}
static HRESULT remote_release(IDirect3DDevice9 *p,struct pw_d3d9_object_ref ref)
{(void)p;(void)ref;if(blocked)return RPC_E_CANTCALLOUT_ININPUTSYNCCALL;releases++;return S_OK;}
static HRESULT defer(IDirect3DDevice9 *p,struct pw_d3d9_deferred *item)
{(void)p;if(defer_failure){rejected=item;return E_OUTOFMEMORY;}if(pending)return E_FAIL;pending=item;return S_OK;}
static void fail(IDirect3DDevice9 *p,HRESULT hr){(void)p;(void)hr;failed++;}
static HRESULT resource(IDirect3DDevice9 *p,struct pw_d3d9_object_ref ref,const struct pw_d3d9_resource_request *q,struct pw_d3d9_resource_reply *r)
{
 (void)p;memset(r,0,sizeof(*r));r->operation=q->operation;
 switch(q->operation){
 case PW_D3D9_RESOURCE_CREATE_VB:case PW_D3D9_RESOURCE_CREATE_IB:
  if(!q->length){r->hresult=(uint32_t)D3DERR_INVALIDCALL;return D3DERR_INVALIDCALL;}
  r->object=(struct pw_d3d9_object_ref){++next_id,1};
  desc[next_id]=(struct pw_d3d9_buffer_desc){q->operation==1?D3DFMT_VERTEXDATA:q->format_fvf,q->operation==1?D3DRTYPE_VERTEXBUFFER:D3DRTYPE_INDEXBUFFER,q->usage,q->pool,q->length,q->operation==1?q->format_fvf:0};
  break;
 case PW_D3D9_RESOURCE_DESC:
  r->desc=desc[ref.id];
  if(release_in_call){release_in_call=0;IDirect3DVertexBuffer9_Release(reentrant);reentrant=NULL;}
  break;
 case PW_D3D9_RESOURCE_LOCK:r->lock_generation=7;r->length=q->length?q->length:desc[ref.id].size-q->offset;break;
 case PW_D3D9_RESOURCE_READ:r->lock_generation=q->lock_generation;r->offset=q->offset;r->count=q->count;memcpy(r->data,bytes+q->offset,q->count);break;
 case PW_D3D9_RESOURCE_WRITE:memcpy(bytes+q->offset,q->data,q->count);break;
 case PW_D3D9_RESOURCE_UNLOCK:case PW_D3D9_RESOURCE_CANCEL_LOCK:break;
 default:r->hresult=(uint32_t)E_NOTIMPL;return E_NOTIMPL;
 }
 return S_OK;
}
int main(void)
{
 IDirect3DDevice9Vtbl vtable={.AddRef=parent_add,.Release=parent_drop};IDirect3DDevice9 parent={&vtable};
 const struct pw_d3d9_buffer_proxy_ops ops={resource,remote_release,defer,fail};
 CHECK(SUCCEEDED(pw_d3d9_buffer_proxy_install(&vtable,&ops)));
 CHECK(SUCCEEDED(pw_d3d9_buffer_proxy_install(&vtable,&ops)));
 struct pw_d3d9_buffer_proxy_ops wrong=ops;wrong.fail=NULL;CHECK(FAILED(pw_d3d9_buffer_proxy_install(&vtable,&wrong)));
 IDirect3DVertexBuffer9 *vb=NULL,*same=NULL;IDirect3DIndexBuffer9 *ib=NULL;void *out=NULL;
 CHECK(SUCCEEDED(IDirect3DDevice9_CreateVertexBuffer(&parent,12000,D3DUSAGE_DYNAMIC,D3DFVF_XYZ,D3DPOOL_DEFAULT,&vb,NULL)));
 CHECK(parent_refs==2&&IDirect3DVertexBuffer9_GetType(vb)==D3DRTYPE_VERTEXBUFFER);
 CHECK(SUCCEEDED(IDirect3DVertexBuffer9_QueryInterface(vb,&IID_IUnknown,&out))&&out==vb);IUnknown_Release((IUnknown *)out);
 CHECK(SUCCEEDED(IDirect3DVertexBuffer9_QueryInterface(vb,&IID_IDirect3DResource9,&out))&&out==vb);IUnknown_Release((IUnknown *)out);
 CHECK(IDirect3DVertexBuffer9_QueryInterface(vb,&IID_IDirect3DIndexBuffer9,&out)==E_NOINTERFACE&&!out);
 IDirect3DDevice9 *got=NULL;CHECK(SUCCEEDED(IDirect3DVertexBuffer9_GetDevice(vb,&got))&&got==&parent);IDirect3DDevice9_Release(got);
 struct pw_d3d9_object_ref ref;CHECK(SUCCEEDED(pw_d3d9_buffer_proxy_resolve(&parent,(IUnknown *)vb,3,&ref))&&ref.id==1);
 CHECK(FAILED(pw_d3d9_buffer_proxy_resolve(&parent,(IUnknown *)(uintptr_t)1,3,&ref)));
 CHECK(FAILED(pw_d3d9_buffer_proxy_resolve(&parent,(IUnknown *)vb,4,&ref)));
 CHECK(SUCCEEDED(pw_d3d9_buffer_proxy_wrap(&parent,3,(struct pw_d3d9_object_ref){1,1},(void **)&same))&&same==vb&&releases==1);IDirect3DVertexBuffer9_Release(same);
 D3DVERTEXBUFFER_DESC vd;CHECK(SUCCEEDED(IDirect3DVertexBuffer9_GetDesc(vb,&vd))&&vd.Size==12000&&vd.FVF==D3DFVF_XYZ);
 CHECK(SUCCEEDED(IDirect3DVertexBuffer9_Lock(vb,0,0,&out,0))&&(uintptr_t)out<=UINT32_MAX);
 memset(out,0x5a,12000);CHECK(SUCCEEDED(IDirect3DVertexBuffer9_Unlock(vb))&&bytes[11999]==0x5a);
 CHECK(SUCCEEDED(IDirect3DVertexBuffer9_Lock(vb,0,12000,&out,D3DLOCK_READONLY))&&((unsigned char *)out)[11999]==0x5a);
 CHECK(SUCCEEDED(IDirect3DVertexBuffer9_Unlock(vb)));
 CHECK(SUCCEEDED(IDirect3DDevice9_CreateIndexBuffer(&parent,8192,0,D3DFMT_INDEX16,D3DPOOL_MANAGED,&ib,NULL)));
 D3DINDEXBUFFER_DESC id;CHECK(SUCCEEDED(IDirect3DIndexBuffer9_GetDesc(ib,&id))&&id.Format==D3DFMT_INDEX16&&id.Type==D3DRTYPE_INDEXBUFFER);
 CHECK(SUCCEEDED(IDirect3DIndexBuffer9_Lock(ib,0,8192,&out,0)));
 blocked=1;CHECK(IDirect3DIndexBuffer9_Release(ib)==0&&pending&&parent_refs==3);
 CHECK(pw_d3d9_buffer_client_staging_bytes()==8192);blocked=0;
 struct pw_d3d9_deferred *task=pending;pending=NULL;task->function(task->context);
 CHECK(parent_refs==2&&pw_d3d9_buffer_client_staging_bytes()==0);
 /* A callback releases the caller's last reference during GetDesc. Its method
  * pin keeps proxy+parent alive until outputs are committed. */
 reentrant=vb;release_in_call=1;CHECK(SUCCEEDED(IDirect3DVertexBuffer9_GetDesc(vb,&vd))&&vd.Size==12000);
 CHECK(parent_refs==1&&releases==3&&!failed&&!pending);
 CHECK(SUCCEEDED(IDirect3DDevice9_CreateVertexBuffer(&parent,1024,0,0,D3DPOOL_DEFAULT,&vb,NULL)));
 blocked=defer_failure=1;CHECK(!IDirect3DVertexBuffer9_Release(vb)&&rejected&&parent_refs==2&&failed==1);
 /* Failed enqueue retained everything. A later safe retry can enqueue again;
  * an unexpectedly still-blocked deferred invocation must also retain/requeue. */
 task=rejected;rejected=NULL;defer_failure=0;task->function(task->context);CHECK(pending==task&&parent_refs==2);
 pending=NULL;task->function(task->context);CHECK(pending==task&&parent_refs==2&&failed==1);
 pending=NULL;blocked=0;task->function(task->context);CHECK(parent_refs==1&&releases==4&&failed==1);
 CHECK(FAILED(IDirect3DDevice9_CreateVertexBuffer(&parent,0,0,0,D3DPOOL_DEFAULT,&vb,NULL))&&!vb&&parent_refs==1);
 puts("PW_BUFFER_PROXY PASS identity=1 low32=1 copied=1 deferred=1 callback_pin=1 foreign_rejected=1");return 0;
}
