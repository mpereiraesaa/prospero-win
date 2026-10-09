/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_device_methods.h"
#include <string.h>
/* All copied structures are pointer-free sequences of four-byte fields (palette
 * entries are four individual bytes). These native ABI bounds are checked in
 * both actual PE32 and PE64 builds. */
_Static_assert(sizeof(float)==4,"float wire layout");
_Static_assert(sizeof(int)==4,"int wire layout");
_Static_assert(sizeof(BOOL)==4,"BOOL wire layout");
_Static_assert(sizeof(DWORD)==4,"DWORD wire layout");
_Static_assert(sizeof(D3DMATRIX)==64,"D3DMATRIX wire layout");
_Static_assert(sizeof(D3DVIEWPORT9)==24,"D3DVIEWPORT9 wire layout");
_Static_assert(sizeof(D3DMATERIAL9)==68,"D3DMATERIAL9 wire layout");
_Static_assert(sizeof(D3DLIGHT9)==104,"D3DLIGHT9 wire layout");
_Static_assert(sizeof(D3DCLIPSTATUS9)==8,"D3DCLIPSTATUS9 wire layout");
_Static_assert(sizeof(D3DRECT)==16,"D3DRECT wire layout");
_Static_assert(sizeof(RECT)==16,"RECT wire layout");
_Static_assert(sizeof(PALETTEENTRY)==4,"PALETTEENTRY wire layout");
_Static_assert(sizeof(D3DDISPLAYMODE)==16,"D3DDISPLAYMODE wire layout");
_Static_assert(sizeof(D3DRASTER_STATUS)==8,"D3DRASTER_STATUS wire layout");
static struct pw_d3d9_device_methods_ops ops;
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
static pw_d3d9_device_binding_fn binding;
void pw_d3d9_device_methods_binding_install(pw_d3d9_device_binding_fn fn){binding=fn;}
#endif
static uint32_t float_bits(float f){uint32_t bits;memcpy(&bits,&f,4);return bits;}
static HRESULT get(IDirect3DDevice9 *device,const struct pw_d3d9_getter_request *q,void *out)
{
 struct pw_d3d9_getter_reply r;size_t bytes;HRESULT hr;
 if(!out || pw_d3d9_getter_bytes(q,&bytes))return D3DERR_INVALIDCALL;
 IDirect3DDevice9_AddRef(device);memset(&r,0,sizeof(r));hr=ops.getter(device,q,&r);
 if(FAILED(hr))goto done;
 if(r.method!=q->method || r.hresult!=(uint32_t)hr || r.bytes!=bytes){ops.fail(device,E_FAIL);hr=E_FAIL;goto done;}
 if(bytes)memcpy(out,r.data.bytes,bytes);
 done:IDirect3DDevice9_Release(device);return hr;
}
static HRESULT WINAPI method_TestCooperativeLevel(IDirect3DDevice9 *device)
{
 struct pw_d3d9_command c={0};c.method=3;
 return ops.command(device,&c);
}
static HRESULT WINAPI method_EvictManagedResources(IDirect3DDevice9 *device)
{
 struct pw_d3d9_command c={0};c.method=5;
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetDialogBoxMode(IDirect3DDevice9 *device, BOOL p0)
{
 struct pw_d3d9_command c={0};c.method=20;
 c.args[0]=!!p0;
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetRenderTarget(IDirect3DDevice9 *device, DWORD p0, IDirect3DSurface9* p1)
{
 struct pw_d3d9_command c={0};c.method=37;
 struct pw_d3d9_object_ref ref={0};HRESULT hr;
 if(p1){hr=ops.resolve(device,(IUnknown *)p1,PW_D3D9_KIND_SURFACE,&ref);if(FAILED(hr))return hr;if(!ref.id || !ref.generation)return D3DERR_INVALIDCALL;}
 c.args[0]=(uint32_t)p0;
 c.args[1]=ref.id;c.args[2]=ref.generation;
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetDepthStencilSurface(IDirect3DDevice9 *device, IDirect3DSurface9* p0)
{
 struct pw_d3d9_command c={0};c.method=39;
 struct pw_d3d9_object_ref ref={0};HRESULT hr;
 if(p0){hr=ops.resolve(device,(IUnknown *)p0,PW_D3D9_KIND_SURFACE,&ref);if(FAILED(hr))return hr;if(!ref.id || !ref.generation)return D3DERR_INVALIDCALL;}
 c.args[0]=ref.id;c.args[1]=ref.generation;
 return ops.command(device,&c);
}
static HRESULT WINAPI method_BeginScene(IDirect3DDevice9 *device)
{
 struct pw_d3d9_command c={0};c.method=41;
 return ops.command(device,&c);
}
static HRESULT WINAPI method_EndScene(IDirect3DDevice9 *device)
{
 struct pw_d3d9_command c={0};c.method=42;
 return ops.command(device,&c);
}
static HRESULT WINAPI method_Clear(IDirect3DDevice9 *device, DWORD p0, const D3DRECT * p1, DWORD p2, D3DCOLOR p3, float p4, DWORD p5)
{
 struct pw_d3d9_command c={0};c.method=43;
 if(p0>256 || (p0 && !p1))return D3DERR_INVALIDCALL;
 c.args[0]=p0;c.args[1]=p2;c.args[2]=p3;c.args[3]=float_bits(p4);c.args[4]=p5;
 c.data_bytes=p0*16;if(c.data_bytes)memcpy(c.data.bytes,p1,c.data_bytes);
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetTransform(IDirect3DDevice9 *device, D3DTRANSFORMSTATETYPE p0, const D3DMATRIX * p1)
{
 struct pw_d3d9_command c={0};c.method=44;
 if(!p1)return D3DERR_INVALIDCALL;
 c.data_bytes=64;memcpy(c.data.bytes,p1,64);
 c.args[0]=(uint32_t)p0;
 return ops.command(device,&c);
}
static HRESULT WINAPI method_MultiplyTransform(IDirect3DDevice9 *device, D3DTRANSFORMSTATETYPE p0, const D3DMATRIX * p1)
{
 struct pw_d3d9_command c={0};c.method=46;
 if(!p1)return D3DERR_INVALIDCALL;
 c.data_bytes=64;memcpy(c.data.bytes,p1,64);
 c.args[0]=(uint32_t)p0;
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetViewport(IDirect3DDevice9 *device, const D3DVIEWPORT9 * p0)
{
 struct pw_d3d9_command c={0};c.method=47;
 if(!p0)return D3DERR_INVALIDCALL;
 c.data_bytes=24;memcpy(c.data.bytes,p0,24);
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetMaterial(IDirect3DDevice9 *device, const D3DMATERIAL9 * p0)
{
 struct pw_d3d9_command c={0};c.method=49;
 if(!p0)return D3DERR_INVALIDCALL;
 c.data_bytes=68;memcpy(c.data.bytes,p0,68);
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetLight(IDirect3DDevice9 *device, DWORD p0, const D3DLIGHT9 * p1)
{
 struct pw_d3d9_command c={0};c.method=51;
 if(!p1)return D3DERR_INVALIDCALL;
 c.data_bytes=104;memcpy(c.data.bytes,p1,104);
 c.args[0]=(uint32_t)p0;
 return ops.command(device,&c);
}
static HRESULT WINAPI method_LightEnable(IDirect3DDevice9 *device, DWORD p0, BOOL p1)
{
 struct pw_d3d9_command c={0};c.method=53;
 c.args[0]=(uint32_t)p0;
 c.args[1]=!!p1;
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetClipPlane(IDirect3DDevice9 *device, DWORD p0, const float * p1)
{
 struct pw_d3d9_command c={0};c.method=55;
 if(!p1)return D3DERR_INVALIDCALL;
 c.data_bytes=16;memcpy(c.data.bytes,p1,16);
 c.args[0]=(uint32_t)p0;
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetRenderState(IDirect3DDevice9 *device, D3DRENDERSTATETYPE p0, DWORD p1)
{
 struct pw_d3d9_command c={0};c.method=57;
 c.args[0]=(uint32_t)p0;
 c.args[1]=(uint32_t)p1;
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetClipStatus(IDirect3DDevice9 *device, const D3DCLIPSTATUS9 * p0)
{
 struct pw_d3d9_command c={0};c.method=62;
 if(!p0)return D3DERR_INVALIDCALL;
 c.data_bytes=8;memcpy(c.data.bytes,p0,8);
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetTexture(IDirect3DDevice9 *device, DWORD p0, IDirect3DBaseTexture9* p1)
{
 struct pw_d3d9_command c={0};c.method=65;
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
 c.args[0]=(uint32_t)p0;
 if(binding)return binding(device,&c,(IUnknown *)p1,PW_D3D9_KIND_TEXTURE_2D,1);
#endif
 struct pw_d3d9_object_ref ref={0};HRESULT hr;
 if(p1){hr=ops.resolve(device,(IUnknown *)p1,PW_D3D9_KIND_TEXTURE_2D,&ref);if(FAILED(hr))return hr;if(!ref.id || !ref.generation)return D3DERR_INVALIDCALL;}
 c.args[0]=(uint32_t)p0;
 c.args[1]=ref.id;c.args[2]=ref.generation;
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetTextureStageState(IDirect3DDevice9 *device, DWORD p0, D3DTEXTURESTAGESTATETYPE p1, DWORD p2)
{
 struct pw_d3d9_command c={0};c.method=67;
 c.args[0]=(uint32_t)p0;
 c.args[1]=(uint32_t)p1;
 c.args[2]=(uint32_t)p2;
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetSamplerState(IDirect3DDevice9 *device, DWORD p0, D3DSAMPLERSTATETYPE p1, DWORD p2)
{
 struct pw_d3d9_command c={0};c.method=69;
 c.args[0]=(uint32_t)p0;
 c.args[1]=(uint32_t)p1;
 c.args[2]=(uint32_t)p2;
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetPaletteEntries(IDirect3DDevice9 *device, UINT p0, const PALETTEENTRY * p1)
{
 struct pw_d3d9_command c={0};c.method=71;
 if(!p1)return D3DERR_INVALIDCALL;
 c.data_bytes=1024;memcpy(c.data.bytes,p1,1024);
 c.args[0]=(uint32_t)p0;
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetCurrentTexturePalette(IDirect3DDevice9 *device, UINT p0)
{
 struct pw_d3d9_command c={0};c.method=73;
 c.args[0]=(uint32_t)p0;
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetScissorRect(IDirect3DDevice9 *device, const RECT * p0)
{
 struct pw_d3d9_command c={0};c.method=75;
 if(!p0)return D3DERR_INVALIDCALL;
 c.data_bytes=16;memcpy(c.data.bytes,p0,16);
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetSoftwareVertexProcessing(IDirect3DDevice9 *device, BOOL p0)
{
 struct pw_d3d9_command c={0};c.method=77;
 c.args[0]=!!p0;
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetNPatchMode(IDirect3DDevice9 *device, float p0)
{
 struct pw_d3d9_command c={0};c.method=79;
 c.args[0]=float_bits(p0);
 return ops.command(device,&c);
}
static HRESULT WINAPI method_DrawPrimitive(IDirect3DDevice9 *device, D3DPRIMITIVETYPE p0, UINT p1, UINT p2)
{
 struct pw_d3d9_command c={0};c.method=81;
 c.args[0]=(uint32_t)p0;
 c.args[1]=(uint32_t)p1;
 c.args[2]=(uint32_t)p2;
 return ops.command(device,&c);
}
static HRESULT WINAPI method_DrawIndexedPrimitive(IDirect3DDevice9 *device, D3DPRIMITIVETYPE p0, INT p1, UINT p2, UINT p3, UINT p4, UINT p5)
{
 struct pw_d3d9_command c={0};c.method=82;
 c.args[0]=(uint32_t)p0;
 c.args[1]=(uint32_t)p1;
 c.args[2]=(uint32_t)p2;
 c.args[3]=(uint32_t)p3;
 c.args[4]=(uint32_t)p4;
 c.args[5]=(uint32_t)p5;
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetVertexDeclaration(IDirect3DDevice9 *device, IDirect3DVertexDeclaration9* p0)
{
 struct pw_d3d9_command c={0};c.method=87;
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS

 if(binding)return binding(device,&c,(IUnknown *)p0,PW_D3D9_KIND_VERTEX_DECLARATION,0);
#endif
 struct pw_d3d9_object_ref ref={0};HRESULT hr;
 if(p0){hr=ops.resolve(device,(IUnknown *)p0,PW_D3D9_KIND_VERTEX_DECLARATION,&ref);if(FAILED(hr))return hr;if(!ref.id || !ref.generation)return D3DERR_INVALIDCALL;}
 c.args[0]=ref.id;c.args[1]=ref.generation;
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetFVF(IDirect3DDevice9 *device, DWORD p0)
{
 struct pw_d3d9_command c={0};c.method=89;
 c.args[0]=(uint32_t)p0;
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetVertexShader(IDirect3DDevice9 *device, IDirect3DVertexShader9* p0)
{
 struct pw_d3d9_command c={0};c.method=92;
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS

 if(binding)return binding(device,&c,(IUnknown *)p0,PW_D3D9_KIND_VERTEX_SHADER,0);
#endif
 struct pw_d3d9_object_ref ref={0};HRESULT hr;
 if(p0){hr=ops.resolve(device,(IUnknown *)p0,PW_D3D9_KIND_VERTEX_SHADER,&ref);if(FAILED(hr))return hr;if(!ref.id || !ref.generation)return D3DERR_INVALIDCALL;}
 c.args[0]=ref.id;c.args[1]=ref.generation;
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetVertexShaderConstantF(IDirect3DDevice9 *device, UINT p0, const float * p1, UINT p2)
{
 struct pw_d3d9_command c={0};c.method=94;
 if(!p1 || p2>256)return D3DERR_INVALIDCALL;
 c.args[0]=p0;c.args[1]=p2;c.data_bytes=p2*16;
 if(c.data_bytes)memcpy(c.data.bytes,p1,c.data_bytes);
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetVertexShaderConstantI(IDirect3DDevice9 *device, UINT p0, const int * p1, UINT p2)
{
 struct pw_d3d9_command c={0};c.method=96;
 if(!p1 || p2>256)return D3DERR_INVALIDCALL;
 c.args[0]=p0;c.args[1]=p2;c.data_bytes=p2*16;
 if(c.data_bytes)memcpy(c.data.bytes,p1,c.data_bytes);
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetVertexShaderConstantB(IDirect3DDevice9 *device, UINT p0, const BOOL * p1, UINT p2)
{
 struct pw_d3d9_command c={0};c.method=98;
 if(!p1 || p2>1024)return D3DERR_INVALIDCALL;
 c.args[0]=p0;c.args[1]=p2;c.data_bytes=p2*4;
 for(UINT i=0;i<p2;i++)c.data.words[i]=!!p1[i];
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetStreamSource(IDirect3DDevice9 *device, UINT p0, IDirect3DVertexBuffer9* p1, UINT p2, UINT p3)
{
 struct pw_d3d9_command c={0};c.method=100;
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
 c.args[0]=(uint32_t)p0;c.args[3]=(uint32_t)p2;c.args[4]=(uint32_t)p3;
 if(binding)return binding(device,&c,(IUnknown *)p1,PW_D3D9_KIND_VERTEX_BUFFER,1);
#endif
 struct pw_d3d9_object_ref ref={0};HRESULT hr;
 if(p1){hr=ops.resolve(device,(IUnknown *)p1,PW_D3D9_KIND_VERTEX_BUFFER,&ref);if(FAILED(hr))return hr;if(!ref.id || !ref.generation)return D3DERR_INVALIDCALL;}
 c.args[0]=(uint32_t)p0;
 c.args[1]=ref.id;c.args[2]=ref.generation;
 c.args[3]=(uint32_t)p2;
 c.args[4]=(uint32_t)p3;
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetStreamSourceFreq(IDirect3DDevice9 *device, UINT p0, UINT p1)
{
 struct pw_d3d9_command c={0};c.method=102;
 c.args[0]=(uint32_t)p0;
 c.args[1]=(uint32_t)p1;
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetIndices(IDirect3DDevice9 *device, IDirect3DIndexBuffer9* p0)
{
 struct pw_d3d9_command c={0};c.method=104;
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS

 if(binding)return binding(device,&c,(IUnknown *)p0,PW_D3D9_KIND_INDEX_BUFFER,0);
#endif
 struct pw_d3d9_object_ref ref={0};HRESULT hr;
 if(p0){hr=ops.resolve(device,(IUnknown *)p0,PW_D3D9_KIND_INDEX_BUFFER,&ref);if(FAILED(hr))return hr;if(!ref.id || !ref.generation)return D3DERR_INVALIDCALL;}
 c.args[0]=ref.id;c.args[1]=ref.generation;
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetPixelShader(IDirect3DDevice9 *device, IDirect3DPixelShader9* p0)
{
 struct pw_d3d9_command c={0};c.method=107;
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS

 if(binding)return binding(device,&c,(IUnknown *)p0,PW_D3D9_KIND_PIXEL_SHADER,0);
#endif
 struct pw_d3d9_object_ref ref={0};HRESULT hr;
 if(p0){hr=ops.resolve(device,(IUnknown *)p0,PW_D3D9_KIND_PIXEL_SHADER,&ref);if(FAILED(hr))return hr;if(!ref.id || !ref.generation)return D3DERR_INVALIDCALL;}
 c.args[0]=ref.id;c.args[1]=ref.generation;
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetPixelShaderConstantF(IDirect3DDevice9 *device, UINT p0, const float * p1, UINT p2)
{
 struct pw_d3d9_command c={0};c.method=109;
 if(!p1 || p2>256)return D3DERR_INVALIDCALL;
 c.args[0]=p0;c.args[1]=p2;c.data_bytes=p2*16;
 if(c.data_bytes)memcpy(c.data.bytes,p1,c.data_bytes);
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetPixelShaderConstantI(IDirect3DDevice9 *device, UINT p0, const int * p1, UINT p2)
{
 struct pw_d3d9_command c={0};c.method=111;
 if(!p1 || p2>256)return D3DERR_INVALIDCALL;
 c.args[0]=p0;c.args[1]=p2;c.data_bytes=p2*16;
 if(c.data_bytes)memcpy(c.data.bytes,p1,c.data_bytes);
 return ops.command(device,&c);
}
static HRESULT WINAPI method_SetPixelShaderConstantB(IDirect3DDevice9 *device, UINT p0, const BOOL * p1, UINT p2)
{
 struct pw_d3d9_command c={0};c.method=113;
 if(!p1 || p2>1024)return D3DERR_INVALIDCALL;
 c.args[0]=p0;c.args[1]=p2;c.data_bytes=p2*4;
 for(UINT i=0;i<p2;i++)c.data.words[i]=!!p1[i];
 return ops.command(device,&c);
}
static UINT WINAPI method_GetAvailableTextureMem(IDirect3DDevice9 *device)
{
 struct pw_d3d9_getter_request q={0};q.method=4;
 UINT value=0;if(FAILED(get(device,&q,&value)))return 0;return value;
}
static HRESULT WINAPI method_GetDisplayMode(IDirect3DDevice9 *device, UINT p0, D3DDISPLAYMODE* p1)
{
 struct pw_d3d9_getter_request q={0};q.method=8;
 q.args[0]=(uint32_t)p0;
 return get(device,&q,p1);
}
static UINT WINAPI method_GetNumberOfSwapChains(IDirect3DDevice9 *device)
{
 struct pw_d3d9_getter_request q={0};q.method=15;
 UINT value=0;if(FAILED(get(device,&q,&value)))return 0;return value;
}
static HRESULT WINAPI method_GetRasterStatus(IDirect3DDevice9 *device, UINT p0, D3DRASTER_STATUS* p1)
{
 struct pw_d3d9_getter_request q={0};q.method=19;
 q.args[0]=(uint32_t)p0;
 return get(device,&q,p1);
}
static HRESULT WINAPI method_GetTransform(IDirect3DDevice9 *device, D3DTRANSFORMSTATETYPE p0, D3DMATRIX* p1)
{
 struct pw_d3d9_getter_request q={0};q.method=45;
 q.args[0]=(uint32_t)p0;
 return get(device,&q,p1);
}
static HRESULT WINAPI method_GetViewport(IDirect3DDevice9 *device, D3DVIEWPORT9* p0)
{
 struct pw_d3d9_getter_request q={0};q.method=48;
 return get(device,&q,p0);
}
static HRESULT WINAPI method_GetMaterial(IDirect3DDevice9 *device, D3DMATERIAL9* p0)
{
 struct pw_d3d9_getter_request q={0};q.method=50;
 return get(device,&q,p0);
}
static HRESULT WINAPI method_GetLight(IDirect3DDevice9 *device, DWORD p0, D3DLIGHT9* p1)
{
 struct pw_d3d9_getter_request q={0};q.method=52;
 q.args[0]=(uint32_t)p0;
 return get(device,&q,p1);
}
static HRESULT WINAPI method_GetLightEnable(IDirect3DDevice9 *device, DWORD p0, BOOL* p1)
{
 struct pw_d3d9_getter_request q={0};q.method=54;
 q.args[0]=(uint32_t)p0;
 return get(device,&q,p1);
}
static HRESULT WINAPI method_GetClipPlane(IDirect3DDevice9 *device, DWORD p0, float* p1)
{
 struct pw_d3d9_getter_request q={0};q.method=56;
 q.args[0]=(uint32_t)p0;
 return get(device,&q,p1);
}
static HRESULT WINAPI method_GetRenderState(IDirect3DDevice9 *device, D3DRENDERSTATETYPE p0, DWORD* p1)
{
 struct pw_d3d9_getter_request q={0};q.method=58;
 q.args[0]=(uint32_t)p0;
 return get(device,&q,p1);
}
static HRESULT WINAPI method_GetClipStatus(IDirect3DDevice9 *device, D3DCLIPSTATUS9* p0)
{
 struct pw_d3d9_getter_request q={0};q.method=63;
 return get(device,&q,p0);
}
static HRESULT WINAPI method_GetTextureStageState(IDirect3DDevice9 *device, DWORD p0, D3DTEXTURESTAGESTATETYPE p1, DWORD* p2)
{
 struct pw_d3d9_getter_request q={0};q.method=66;
 q.args[0]=(uint32_t)p0;
 q.args[1]=(uint32_t)p1;
 return get(device,&q,p2);
}
static HRESULT WINAPI method_GetSamplerState(IDirect3DDevice9 *device, DWORD p0, D3DSAMPLERSTATETYPE p1, DWORD* p2)
{
 struct pw_d3d9_getter_request q={0};q.method=68;
 q.args[0]=(uint32_t)p0;
 q.args[1]=(uint32_t)p1;
 return get(device,&q,p2);
}
static HRESULT WINAPI method_ValidateDevice(IDirect3DDevice9 *device, DWORD* p0)
{
 struct pw_d3d9_getter_request q={0};q.method=70;
 return get(device,&q,p0);
}
static HRESULT WINAPI method_GetPaletteEntries(IDirect3DDevice9 *device, UINT p0, PALETTEENTRY* p1)
{
 struct pw_d3d9_getter_request q={0};q.method=72;
 q.args[0]=(uint32_t)p0;
 return get(device,&q,p1);
}
static HRESULT WINAPI method_GetCurrentTexturePalette(IDirect3DDevice9 *device, UINT * p0)
{
 struct pw_d3d9_getter_request q={0};q.method=74;
 return get(device,&q,p0);
}
static HRESULT WINAPI method_GetScissorRect(IDirect3DDevice9 *device, RECT* p0)
{
 struct pw_d3d9_getter_request q={0};q.method=76;
 return get(device,&q,p0);
}
static BOOL WINAPI method_GetSoftwareVertexProcessing(IDirect3DDevice9 *device)
{
 struct pw_d3d9_getter_request q={0};q.method=78;
 BOOL value=0;if(FAILED(get(device,&q,&value)))return 0;return value;
}
static float WINAPI method_GetNPatchMode(IDirect3DDevice9 *device)
{
 struct pw_d3d9_getter_request q={0};q.method=80;
 float value=0;if(FAILED(get(device,&q,&value)))return 0;return value;
}
static HRESULT WINAPI method_GetFVF(IDirect3DDevice9 *device, DWORD* p0)
{
 struct pw_d3d9_getter_request q={0};q.method=90;
 return get(device,&q,p0);
}
static HRESULT WINAPI method_GetVertexShaderConstantF(IDirect3DDevice9 *device, UINT p0, float* p1, UINT p2)
{
 struct pw_d3d9_getter_request q={0};q.method=95;
 q.args[0]=(uint32_t)p0;
 q.args[1]=(uint32_t)p2;
 return get(device,&q,p1);
}
static HRESULT WINAPI method_GetVertexShaderConstantI(IDirect3DDevice9 *device, UINT p0, int* p1, UINT p2)
{
 struct pw_d3d9_getter_request q={0};q.method=97;
 q.args[0]=(uint32_t)p0;
 q.args[1]=(uint32_t)p2;
 return get(device,&q,p1);
}
static HRESULT WINAPI method_GetVertexShaderConstantB(IDirect3DDevice9 *device, UINT p0, BOOL* p1, UINT p2)
{
 struct pw_d3d9_getter_request q={0};q.method=99;
 q.args[0]=(uint32_t)p0;
 q.args[1]=(uint32_t)p2;
 return get(device,&q,p1);
}
static HRESULT WINAPI method_GetStreamSourceFreq(IDirect3DDevice9 *device, UINT p0, UINT* p1)
{
 struct pw_d3d9_getter_request q={0};q.method=103;
 q.args[0]=(uint32_t)p0;
 return get(device,&q,p1);
}
static HRESULT WINAPI method_GetPixelShaderConstantF(IDirect3DDevice9 *device, UINT p0, float* p1, UINT p2)
{
 struct pw_d3d9_getter_request q={0};q.method=110;
 q.args[0]=(uint32_t)p0;
 q.args[1]=(uint32_t)p2;
 return get(device,&q,p1);
}
static HRESULT WINAPI method_GetPixelShaderConstantI(IDirect3DDevice9 *device, UINT p0, int* p1, UINT p2)
{
 struct pw_d3d9_getter_request q={0};q.method=112;
 q.args[0]=(uint32_t)p0;
 q.args[1]=(uint32_t)p2;
 return get(device,&q,p1);
}
static HRESULT WINAPI method_GetPixelShaderConstantB(IDirect3DDevice9 *device, UINT p0, BOOL* p1, UINT p2)
{
 struct pw_d3d9_getter_request q={0};q.method=114;
 q.args[0]=(uint32_t)p0;
 q.args[1]=(uint32_t)p2;
 return get(device,&q,p1);
}
void pw_d3d9_device_methods_install(IDirect3DDevice9Vtbl *table,const struct pw_d3d9_device_methods_ops *callbacks)
{
 ops=*callbacks;
 table->TestCooperativeLevel=method_TestCooperativeLevel;
 table->EvictManagedResources=method_EvictManagedResources;
 table->SetDialogBoxMode=method_SetDialogBoxMode;
 table->SetRenderTarget=method_SetRenderTarget;
 table->SetDepthStencilSurface=method_SetDepthStencilSurface;
 table->BeginScene=method_BeginScene;
 table->EndScene=method_EndScene;
 table->Clear=method_Clear;
 table->SetTransform=method_SetTransform;
 table->MultiplyTransform=method_MultiplyTransform;
 table->SetViewport=method_SetViewport;
 table->SetMaterial=method_SetMaterial;
 table->SetLight=method_SetLight;
 table->LightEnable=method_LightEnable;
 table->SetClipPlane=method_SetClipPlane;
 table->SetRenderState=method_SetRenderState;
 table->SetClipStatus=method_SetClipStatus;
 table->SetTexture=method_SetTexture;
 table->SetTextureStageState=method_SetTextureStageState;
 table->SetSamplerState=method_SetSamplerState;
 table->SetPaletteEntries=method_SetPaletteEntries;
 table->SetCurrentTexturePalette=method_SetCurrentTexturePalette;
 table->SetScissorRect=method_SetScissorRect;
 table->SetSoftwareVertexProcessing=method_SetSoftwareVertexProcessing;
 table->SetNPatchMode=method_SetNPatchMode;
 table->DrawPrimitive=method_DrawPrimitive;
 table->DrawIndexedPrimitive=method_DrawIndexedPrimitive;
 table->SetVertexDeclaration=method_SetVertexDeclaration;
 table->SetFVF=method_SetFVF;
 table->SetVertexShader=method_SetVertexShader;
 table->SetVertexShaderConstantF=method_SetVertexShaderConstantF;
 table->SetVertexShaderConstantI=method_SetVertexShaderConstantI;
 table->SetVertexShaderConstantB=method_SetVertexShaderConstantB;
 table->SetStreamSource=method_SetStreamSource;
 table->SetStreamSourceFreq=method_SetStreamSourceFreq;
 table->SetIndices=method_SetIndices;
 table->SetPixelShader=method_SetPixelShader;
 table->SetPixelShaderConstantF=method_SetPixelShaderConstantF;
 table->SetPixelShaderConstantI=method_SetPixelShaderConstantI;
 table->SetPixelShaderConstantB=method_SetPixelShaderConstantB;
 table->GetAvailableTextureMem=method_GetAvailableTextureMem;
 table->GetDisplayMode=method_GetDisplayMode;
 table->GetNumberOfSwapChains=method_GetNumberOfSwapChains;
 table->GetRasterStatus=method_GetRasterStatus;
 table->GetTransform=method_GetTransform;
 table->GetViewport=method_GetViewport;
 table->GetMaterial=method_GetMaterial;
 table->GetLight=method_GetLight;
 table->GetLightEnable=method_GetLightEnable;
 table->GetClipPlane=method_GetClipPlane;
 table->GetRenderState=method_GetRenderState;
 table->GetClipStatus=method_GetClipStatus;
 table->GetTextureStageState=method_GetTextureStageState;
 table->GetSamplerState=method_GetSamplerState;
 table->ValidateDevice=method_ValidateDevice;
 table->GetPaletteEntries=method_GetPaletteEntries;
 table->GetCurrentTexturePalette=method_GetCurrentTexturePalette;
 table->GetScissorRect=method_GetScissorRect;
 table->GetSoftwareVertexProcessing=method_GetSoftwareVertexProcessing;
 table->GetNPatchMode=method_GetNPatchMode;
 table->GetFVF=method_GetFVF;
 table->GetVertexShaderConstantF=method_GetVertexShaderConstantF;
 table->GetVertexShaderConstantI=method_GetVertexShaderConstantI;
 table->GetVertexShaderConstantB=method_GetVertexShaderConstantB;
 table->GetStreamSourceFreq=method_GetStreamSourceFreq;
 table->GetPixelShaderConstantF=method_GetPixelShaderConstantF;
 table->GetPixelShaderConstantI=method_GetPixelShaderConstantI;
 table->GetPixelShaderConstantB=method_GetPixelShaderConstantB;
}
