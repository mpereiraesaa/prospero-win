/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_device_object_methods.h"
static struct pw_d3d9_device_object_methods_ops ops;
static HRESULT get(IDirect3DDevice9 *d,const struct pw_d3d9_object_getter_request *q,void **object,UINT *offset,UINT *stride)
{
 struct pw_d3d9_object_getter_reply r={0};unsigned char wire[32];size_t bytes;void *local=NULL;HRESULT hr,wrapped;
 IDirect3DDevice9_AddRef(d);hr=ops.getter(d,q,&r);if(FAILED(hr))goto done;
 if(r.hresult!=(uint32_t)hr || pw_d3d9_object_getter_reply_encode(wire,sizeof(wire),&bytes,q,&r)){
  ops.fail(d,E_FAIL);hr=E_FAIL;goto done;
 }
 if(r.id){wrapped=ops.wrap(d,r.kind,(struct pw_d3d9_object_ref){r.id,r.generation},&local);if(FAILED(wrapped)){hr=wrapped;goto done;}if(!local){ops.fail(d,E_FAIL);hr=E_FAIL;goto done;}}
 *object=local;if(offset)*offset=r.offset;if(stride)*stride=r.stride;
 done:IDirect3DDevice9_Release(d);return hr;
}
static HRESULT WINAPI backbuffer(IDirect3DDevice9 *d,UINT chain,UINT index,D3DBACKBUFFER_TYPE type,IDirect3DSurface9 **out)
{struct pw_d3d9_object_getter_request q={18,{chain,index,type}};void *object;HRESULT hr;if(!out)return D3DERR_INVALIDCALL;hr=get(d,&q,&object,NULL,NULL);if(SUCCEEDED(hr))*out=object;return hr;}
static HRESULT WINAPI render_target(IDirect3DDevice9 *d,DWORD index,IDirect3DSurface9 **out)
{struct pw_d3d9_object_getter_request q={38,{index}};void *object;HRESULT hr;if(!out)return D3DERR_INVALIDCALL;hr=get(d,&q,&object,NULL,NULL);if(SUCCEEDED(hr))*out=object;return hr;}
static HRESULT WINAPI depth(IDirect3DDevice9 *d,IDirect3DSurface9 **out)
{struct pw_d3d9_object_getter_request q={40,{0}};void *object;HRESULT hr;if(!out)return D3DERR_INVALIDCALL;hr=get(d,&q,&object,NULL,NULL);if(SUCCEEDED(hr))*out=object;return hr;}
static HRESULT WINAPI texture(IDirect3DDevice9 *d,DWORD stage,IDirect3DBaseTexture9 **out)
{struct pw_d3d9_object_getter_request q={64,{stage}};void *object;HRESULT hr;if(!out)return D3DERR_INVALIDCALL;hr=get(d,&q,&object,NULL,NULL);if(SUCCEEDED(hr))*out=object;return hr;}
static HRESULT WINAPI declaration(IDirect3DDevice9 *d,IDirect3DVertexDeclaration9 **out)
{struct pw_d3d9_object_getter_request q={88,{0}};void *object;HRESULT hr;if(!out)return D3DERR_INVALIDCALL;hr=get(d,&q,&object,NULL,NULL);if(SUCCEEDED(hr))*out=object;return hr;}
static HRESULT WINAPI vertex_shader(IDirect3DDevice9 *d,IDirect3DVertexShader9 **out)
{struct pw_d3d9_object_getter_request q={93,{0}};void *object;HRESULT hr;if(!out)return D3DERR_INVALIDCALL;hr=get(d,&q,&object,NULL,NULL);if(SUCCEEDED(hr))*out=object;return hr;}
static HRESULT WINAPI stream(IDirect3DDevice9 *d,UINT index,IDirect3DVertexBuffer9 **out,UINT *offset,UINT *stride)
{struct pw_d3d9_object_getter_request q={101,{index}};void *object;UINT actual_offset,actual_stride;HRESULT hr;if(!out || !offset || !stride)return D3DERR_INVALIDCALL;hr=get(d,&q,&object,&actual_offset,&actual_stride);if(SUCCEEDED(hr)){*out=object;*offset=actual_offset;*stride=actual_stride;}return hr;}
static HRESULT WINAPI indices(IDirect3DDevice9 *d,IDirect3DIndexBuffer9 **out)
{struct pw_d3d9_object_getter_request q={105,{0}};void *object;HRESULT hr;if(!out)return D3DERR_INVALIDCALL;hr=get(d,&q,&object,NULL,NULL);if(SUCCEEDED(hr))*out=object;return hr;}
static HRESULT WINAPI pixel_shader(IDirect3DDevice9 *d,IDirect3DPixelShader9 **out)
{struct pw_d3d9_object_getter_request q={108,{0}};void *object;HRESULT hr;if(!out)return D3DERR_INVALIDCALL;hr=get(d,&q,&object,NULL,NULL);if(SUCCEEDED(hr))*out=object;return hr;}
void pw_d3d9_device_object_methods_install(IDirect3DDevice9Vtbl *table,const struct pw_d3d9_device_object_methods_ops *callbacks)
{ops=*callbacks;table->GetBackBuffer=backbuffer;table->GetRenderTarget=render_target;table->GetDepthStencilSurface=depth;table->GetTexture=texture;table->GetVertexDeclaration=declaration;table->GetVertexShader=vertex_shader;table->GetStreamSource=stream;table->GetIndices=indices;table->GetPixelShader=pixel_shader;}
