/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_native_query.h"
#include <string.h>
struct pw_d3d9_native_query {IDirect3DQuery9 *object;IDirect3DDevice9 *parent;IUnknown *identity;uint32_t type,size;};
static int valid(const struct pw_d3d9_query_request *q)
{unsigned char wire[PW_D3D9_QUERY_WIRE_MAX];size_t length;return !pw_d3d9_query_request_encode(wire,sizeof(wire),&length,q);}
void pw_d3d9_native_query_destroy(struct pw_d3d9_native_query *p)
{if(p){if(p->identity)IUnknown_Release(p->identity);if(p->object)IDirect3DQuery9_Release(p->object);if(p->parent)IDirect3DDevice9_Release(p->parent);HeapFree(GetProcessHeap(),0,p);}}
IUnknown *pw_d3d9_native_query_identity(struct pw_d3d9_native_query *p){return p?p->identity:NULL;}
void pw_d3d9_native_query_create(IDirect3DDevice9 *device,const struct pw_d3d9_query_request *q,struct pw_d3d9_query_reply *r,struct pw_d3d9_native_query **out)
{
 struct pw_d3d9_native_query *p=NULL;HRESULT hr=D3DERR_INVALIDCALL,created;IDirect3DDevice9 *actual=NULL;
 memset(r,0,sizeof(*r));r->method=q->method;if(out)*out=NULL;
 if(!out || !device || !valid(q) || q->method!=PW_D3D9_QUERY_CREATE)goto done;
 if(!pw_d3d9_query_type_size(q->type)){hr=D3DERR_NOTAVAILABLE;goto done;}
 if(!q->want_object){hr=IDirect3DDevice9_CreateQuery(device,q->type,NULL);goto done;}
 p=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*p));if(!p){hr=E_OUTOFMEMORY;goto done;}
 created=hr=IDirect3DDevice9_CreateQuery(device,q->type,&p->object);if(FAILED(hr))goto done;
 if(!p->object){hr=E_FAIL;goto done;}
 p->type=IDirect3DQuery9_GetType(p->object);p->size=IDirect3DQuery9_GetDataSize(p->object);
 if(p->type!=q->type || p->size!=pw_d3d9_query_type_size(p->type)){hr=E_FAIL;goto done;}
 hr=IDirect3DQuery9_GetDevice(p->object,&actual);if(FAILED(hr))goto done;
 if(actual!=device){hr=E_FAIL;goto done;}
 hr=IDirect3DQuery9_QueryInterface(p->object,&IID_IUnknown,(void **)&p->identity);if(FAILED(hr))goto done;
 if(!p->identity){hr=E_FAIL;goto done;}
 p->parent=actual;actual=NULL;r->type=p->type;r->size=p->size;*out=p;p=NULL;hr=created;
 done:if(actual)IDirect3DDevice9_Release(actual);pw_d3d9_native_query_destroy(p);r->hresult=(uint32_t)hr;
}
void pw_d3d9_native_query_call(struct pw_d3d9_native_query *p,const struct pw_d3d9_query_request *q,struct pw_d3d9_query_reply *r)
{
 /* DXVK EVENT can write BOOL even for a nonnull zero-size probe. Keep a full
  * scratch result but publish only the caller-declared bytes. Seed bytes also
  * preserve cached EVENT's one-byte update and S_FALSE's partial writes. */
 union {uint64_t align;unsigned char bytes[PW_D3D9_QUERY_DATA_MAX];} scratch={0};HRESULT hr=D3DERR_INVALIDCALL;
 memset(r,0,sizeof(*r));r->method=q->method;
 if(q->method==PW_D3D9_QUERY_DATA && q->has_data==1 && q->size<=PW_D3D9_QUERY_DATA_MAX){r->count=q->size;memcpy(r->data,q->data,r->count);}
 if(!p || !valid(q))goto done;
 if(q->method==PW_D3D9_QUERY_ISSUE)hr=IDirect3DQuery9_Issue(p->object,q->flags);
 else if(q->method==PW_D3D9_QUERY_DATA){
  if(q->size>p->size)goto done;
  memcpy(scratch.bytes,q->data,r->count);
  hr=IDirect3DQuery9_GetData(p->object,q->has_data?scratch.bytes:NULL,q->size,q->flags);
  memcpy(r->data,scratch.bytes,r->count);
 }
 done:r->hresult=(uint32_t)hr;
}
