/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_native_up.h"
static HRESULT native_bound(IDirect3DDevice9 *device,const struct pw_d3d9_up_draw *d)
{
 static const unsigned sizes[17]={4,8,12,16,4,4,4,8,4,4,8,4,8,4,4,4,8};
 IDirect3DVertexDeclaration9 *decl=NULL;D3DVERTEXELEMENT9 elements[65];UINT count=65,i,extent=0;uint64_t vertices,bytes;HRESULT hr;
 hr=IDirect3DDevice9_GetVertexDeclaration(device,&decl);
 if(FAILED(hr))return hr;
 if(!decl)return D3DERR_INVALIDCALL;
 hr=IDirect3DVertexDeclaration9_GetDeclaration(decl,elements,&count);
 IDirect3DVertexDeclaration9_Release(decl);
 if(FAILED(hr))return hr;
 if(!count||count>65||elements[count-1].Stream!=0xff)return D3DERR_INVALIDCALL;
 for(i=0;i+1<count;i++){
  unsigned end;
  if(elements[i].Type>=17)return D3DERR_INVALIDCALL;
  if(elements[i].Stream)continue;
  end=elements[i].Offset+sizes[elements[i].Type];if(end>extent)extent=end;
 }
 if(d->method==84)vertices=(uint64_t)d->min_vertex+d->num_vertices;
 else switch(d->primitive_type){
 case 1:vertices=d->primitive_count;break;case 2:vertices=(uint64_t)d->primitive_count*2;break;
 case 3:vertices=(uint64_t)d->primitive_count+1;break;case 4:vertices=(uint64_t)d->primitive_count*3;break;
 default:vertices=(uint64_t)d->primitive_count+2;break;
 }
 bytes=(vertices-1)*d->stride+(extent>d->stride?extent:d->stride)+d->index_bytes;
 return bytes>PW_D3D9_UP_LIMIT?D3DERR_INVALIDCALL:S_OK;
}
HRESULT pw_d3d9_native_up_dispatch(IDirect3DDevice9 *device,const struct pw_d3d9_up_upload *u)
{
 struct pw_d3d9_up_draw d;unsigned char dummy[16]={0};const void *vertices,*indices;HRESULT hr;
 if(!device||!u||!u->transfer||!u->ready||u->received!=u->total)return D3DERR_INVALIDCALL;
 d=u->draw;
 if(pw_d3d9_up_measure(&d)||d.vertex_bytes!=u->draw.vertex_bytes||d.index_bytes!=u->draw.index_bytes||u->total!=d.vertex_bytes+d.index_bytes||u->capacity<u->total||(u->total&&!u->storage))return D3DERR_INVALIDCALL;
 /* Zero primitives go directly to the backend: it checks declaration before
  * returning and must preserve its exact result and binding behavior. */
 if(d.primitive_count){hr=native_bound(device,&d);if(FAILED(hr))return hr;}
 vertices=d.vertex_bytes?u->storage:dummy;
 indices=d.index_bytes?u->storage+d.vertex_bytes:dummy;
 if(d.method==83)return IDirect3DDevice9_DrawPrimitiveUP(device,d.primitive_type,d.primitive_count,vertices,d.stride);
 return IDirect3DDevice9_DrawIndexedPrimitiveUP(device,d.primitive_type,d.min_vertex,d.num_vertices,d.primitive_count,indices,d.index_format,vertices,d.stride);
}
