/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_up_client.h"
#include <string.h>
static struct pw_d3d9_up_client_ops ops;
static LONG owned_bytes;
static HRESULT call(IDirect3DDevice9 *device,struct pw_d3d9_up_request *q,uint64_t *token)
{
 struct pw_d3d9_up_reply r={0};unsigned char wire[24];size_t n;HRESULT hr=ops.up(device,q,&r);
 if(FAILED(hr))return hr;
 if(r.operation!=q->operation||r.hresult!=(uint32_t)hr||pw_d3d9_up_reply_encode(wire,sizeof(wire),&n,&r)||(q->transfer&&r.transfer!=q->transfer)){
  ops.fail(device,E_FAIL);return E_FAIL;
 }
 *token=r.transfer;return hr;
}
static HRESULT submit(IDirect3DDevice9 *device,struct pw_d3d9_up_draw draw,const void *vertices,const void *indices)
{
 struct pw_d3d9_up_request q={0};unsigned char *snapshot=NULL;uint32_t total,offset;LONG current;uint64_t token=0,ignored;HRESULT hr;
 if(pw_d3d9_up_measure(&draw))return D3DERR_INVALIDCALL;
 total=draw.vertex_bytes+draw.index_bytes;
 if((draw.vertex_bytes&&(!vertices||(uintptr_t)vertices>UINTPTR_MAX-draw.vertex_bytes))||(draw.index_bytes&&(!indices||(uintptr_t)indices>UINTPTR_MAX-draw.index_bytes)))return D3DERR_INVALIDCALL;
 IDirect3DDevice9_AddRef(device);
 if(total){
  do{current=InterlockedCompareExchange(&owned_bytes,0,0);if((uint32_t)current>PW_D3D9_UP_LIMIT-total){hr=E_OUTOFMEMORY;goto done;}}while(InterlockedCompareExchange(&owned_bytes,current+(LONG)total,current)!=current);
  snapshot=HeapAlloc(GetProcessHeap(),0,total);
  if(!snapshot){InterlockedExchangeAdd(&owned_bytes,-(LONG)total);hr=E_OUTOFMEMORY;goto done;}
  if(draw.vertex_bytes)memcpy(snapshot,vertices,draw.vertex_bytes);
  if(draw.index_bytes)memcpy(snapshot+draw.vertex_bytes,indices,draw.index_bytes);
 }
 q.operation=PW_D3D9_UP_BEGIN;q.draw=draw;hr=call(device,&q,&token);if(FAILED(hr))goto done;
 memset(&q,0,sizeof(q));q.operation=PW_D3D9_UP_WRITE;q.transfer=token;
 for(offset=0;offset<total;offset+=q.count){
  q.offset=offset;q.count=total-offset;if(q.count>PW_D3D9_UP_CHUNK)q.count=PW_D3D9_UP_CHUNK;
  memcpy(q.data,snapshot+offset,q.count);hr=call(device,&q,&ignored);
  if(FAILED(hr)){
   memset(&q,0,sizeof(q));q.operation=PW_D3D9_UP_ABORT;q.transfer=token;
   if(FAILED(call(device,&q,&ignored)))ops.fail(device,E_FAIL);
   goto done;
  }
 }
 memset(&q,0,sizeof(q));q.operation=PW_D3D9_UP_COMMIT;q.transfer=token;
 /* Service consumes committed upload even when native draw fails. */
 hr=call(device,&q,&ignored);
 done:
 if(snapshot){HeapFree(GetProcessHeap(),0,snapshot);InterlockedExchangeAdd(&owned_bytes,-(LONG)total);}
 IDirect3DDevice9_Release(device);return hr;
}
static HRESULT WINAPI primitive(IDirect3DDevice9 *device,D3DPRIMITIVETYPE type,UINT count,const void *vertices,UINT stride)
{return submit(device,(struct pw_d3d9_up_draw){83,type,0,0,count,stride,0,0,0},vertices,NULL);}
static HRESULT WINAPI indexed(IDirect3DDevice9 *device,D3DPRIMITIVETYPE type,UINT min_vertex,UINT num_vertices,UINT count,const void *indices,D3DFORMAT format,const void *vertices,UINT stride)
{return submit(device,(struct pw_d3d9_up_draw){84,type,min_vertex,num_vertices,count,stride,format,0,0},vertices,indices);}
void pw_d3d9_up_client_install(IDirect3DDevice9Vtbl *table,const struct pw_d3d9_up_client_ops *callbacks)
{ops=*callbacks;table->DrawPrimitiveUP=primitive;table->DrawIndexedPrimitiveUP=indexed;}
