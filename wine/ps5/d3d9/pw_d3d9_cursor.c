/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_cursor.h"
#include <string.h>
static struct pw_d3d9_cursor_ops ops;
HRESULT pw_d3d9_native_cursor(IDirect3DDevice9 *device,const struct pw_d3d9_cursor_request *q,struct pw_d3d9_cursor_reply *r,pw_d3d9_cursor_acquire_fn acquire,void *context)
{
 unsigned char wire[32];size_t n;IDirect3DSurface9 *surface=NULL;HRESULT hr=S_OK;INT x,y;BOOL show;
 if(!r)return E_POINTER;
 *r=(struct pw_d3d9_cursor_reply){q?q->method:0,(uint32_t)D3DERR_INVALIDCALL,0};
 if(!device||pw_d3d9_cursor_encode(wire,sizeof(wire),&n,q))return D3DERR_INVALIDCALL;
 if(q->method==10){
  if(q->id){if(!acquire)return D3DERR_INVALIDCALL;hr=acquire(context,q->id,q->generation,PW_D3D9_KIND_SURFACE,device,(void **)&surface);if(FAILED(hr))goto done;if(!surface){hr=E_FAIL;goto done;}}
  hr=IDirect3DDevice9_SetCursorProperties(device,q->arg0,q->arg1,surface);
  if(surface)IDirect3DSurface9_Release(surface);
 }else if(q->method==11){memcpy(&x,&q->arg0,4);memcpy(&y,&q->arg1,4);IDirect3DDevice9_SetCursorPosition(device,x,y,q->flags);}
 else {memcpy(&show,&q->arg0,4);show=IDirect3DDevice9_ShowCursor(device,show);memcpy(&r->value,&show,4);}
 done:r->hresult=(uint32_t)hr;return hr;
}
static HRESULT perform(IDirect3DDevice9 *device,struct pw_d3d9_cursor_request *q,IDirect3DSurface9 *surface,uint32_t *value)
{
 struct pw_d3d9_cursor_reply r={0};struct pw_d3d9_object_ref ref;unsigned char wire[16];size_t n;HRESULT hr;
 IDirect3DDevice9_AddRef(device);
 if(surface){hr=ops.resolve(device,(IUnknown *)surface,PW_D3D9_KIND_SURFACE,&ref);if(FAILED(hr))goto done;q->id=ref.id;q->generation=ref.generation;}
 hr=ops.call(device,q,&r);
 if(FAILED(hr)){if(q->method!=10)ops.fail(device,hr);goto done;}
 if(r.method!=q->method||r.hresult!=(uint32_t)hr||pw_d3d9_cursor_reply_encode(wire,sizeof(wire),&n,&r)){ops.fail(device,E_FAIL);hr=E_FAIL;goto done;}
 if(value)*value=r.value;
 done:IDirect3DDevice9_Release(device);return hr;
}
static HRESULT WINAPI properties(IDirect3DDevice9 *d,UINT x,UINT y,IDirect3DSurface9 *surface)
{struct pw_d3d9_cursor_request q={10,x,y,0,0,0};return perform(d,&q,surface,NULL);}
static void WINAPI position(IDirect3DDevice9 *d,INT x,INT y,DWORD flags)
{struct pw_d3d9_cursor_request q={11,(uint32_t)x,(uint32_t)y,flags,0,0};perform(d,&q,NULL,NULL);}
static BOOL WINAPI show(IDirect3DDevice9 *d,BOOL value)
{struct pw_d3d9_cursor_request q={12,(uint32_t)value,0,0,0,0};uint32_t bits=0;BOOL result;perform(d,&q,NULL,&bits);memcpy(&result,&bits,4);return result;}
void pw_d3d9_cursor_install(IDirect3DDevice9Vtbl *table,const struct pw_d3d9_cursor_ops *callbacks)
{ops=*callbacks;table->SetCursorProperties=properties;table->SetCursorPosition=position;table->ShowCursor=show;}
