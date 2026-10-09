/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Controlled native COM ABI proof: all forty reviewed methods. */
#define COBJMACROS
#include "pw_d3d9_native_command.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct pw_d3d9_command expected;
static unsigned calls,acquires,releases,held,deny,null_binding;
static IDirect3DDevice9 device;
static IUnknown object;
static ULONG STDMETHODCALLTYPE release_object(IUnknown *self)
{assert(self==&object && held==1);held=0;++releases;return 1;}
static const IUnknownVtbl object_vtable={.Release=release_object};
static HRESULT acquire_object(void *context,uint32_t id,uint32_t generation,uint32_t kind,IDirect3DDevice9 *owner,void **out)
{
 unsigned expected_kind=0;assert(context==&expected && owner==&device && id==17 && generation==23);
 switch(expected.method){case 37:case 39:expected_kind=PW_D3D9_KIND_SURFACE;break;
 case 65:expected_kind=PW_D3D9_KIND_TEXTURE_2D;break;case 87:expected_kind=PW_D3D9_KIND_VERTEX_DECLARATION;break;
 case 92:expected_kind=PW_D3D9_KIND_VERTEX_SHADER;break;case 100:expected_kind=PW_D3D9_KIND_VERTEX_BUFFER;break;
 case 104:expected_kind=PW_D3D9_KIND_INDEX_BUFFER;break;case 107:expected_kind=PW_D3D9_KIND_PIXEL_SHADER;break;}
 assert(kind==expected_kind && kind);++acquires;if(deny)return D3DERR_DEVICELOST;
 assert(!held);held=1;*out=&object;return S_OK;
}
static uint32_t float_bits(float f){uint32_t u;memcpy(&u,&f,4);return u;}
static HRESULT result(unsigned method){return (HRESULT)(0x88760000u+method);}
static HRESULT STDMETHODCALLTYPE test_TestCooperativeLevel(IDirect3DDevice9 *self)
{assert(self==&device && expected.method==3);++calls;return result(3);}
static HRESULT STDMETHODCALLTYPE test_EvictManagedResources(IDirect3DDevice9 *self)
{assert(self==&device && expected.method==5);++calls;return result(5);}
static HRESULT STDMETHODCALLTYPE test_SetDialogBoxMode(IDirect3DDevice9 *self, BOOL bEnableDialogs)
{assert(self==&device && expected.method==20);++calls;assert((uint32_t)bEnableDialogs==expected.args[0]);return result(20);}
static HRESULT STDMETHODCALLTYPE test_SetRenderTarget(IDirect3DDevice9 *self, DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget)
{assert(self==&device && expected.method==37);++calls;assert((uint32_t)RenderTargetIndex==expected.args[0]);assert((void *)pRenderTarget==(null_binding?NULL:(void *)&object));return result(37);}
static HRESULT STDMETHODCALLTYPE test_SetDepthStencilSurface(IDirect3DDevice9 *self, IDirect3DSurface9* pNewZStencil)
{assert(self==&device && expected.method==39);++calls;assert((void *)pNewZStencil==(null_binding?NULL:(void *)&object));return result(39);}
static HRESULT STDMETHODCALLTYPE test_BeginScene(IDirect3DDevice9 *self)
{assert(self==&device && expected.method==41);++calls;return result(41);}
static HRESULT STDMETHODCALLTYPE test_EndScene(IDirect3DDevice9 *self)
{assert(self==&device && expected.method==42);++calls;return result(42);}
static HRESULT STDMETHODCALLTYPE test_Clear(IDirect3DDevice9 *self, DWORD rect_count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{assert(self==&device && expected.method==43);++calls;assert((uint32_t)rect_count==expected.args[0]);assert(!expected.data_bytes || (rects && !memcmp(rects,expected.data.bytes,expected.data_bytes)));assert((uint32_t)flags==expected.args[1]);assert((uint32_t)color==expected.args[2]);assert(float_bits(z)==expected.args[3]);assert((uint32_t)stencil==expected.args[4]);return result(43);}
static HRESULT STDMETHODCALLTYPE test_SetTransform(IDirect3DDevice9 *self, D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix)
{assert(self==&device && expected.method==44);++calls;assert((uint32_t)state==expected.args[0]);assert(!expected.data_bytes || (matrix && !memcmp(matrix,expected.data.bytes,expected.data_bytes)));return result(44);}
static HRESULT STDMETHODCALLTYPE test_MultiplyTransform(IDirect3DDevice9 *self, D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix)
{assert(self==&device && expected.method==46);++calls;assert((uint32_t)state==expected.args[0]);assert(!expected.data_bytes || (matrix && !memcmp(matrix,expected.data.bytes,expected.data_bytes)));return result(46);}
static HRESULT STDMETHODCALLTYPE test_SetViewport(IDirect3DDevice9 *self, const D3DVIEWPORT9 *viewport)
{assert(self==&device && expected.method==47);++calls;assert(!expected.data_bytes || (viewport && !memcmp(viewport,expected.data.bytes,expected.data_bytes)));return result(47);}
static HRESULT STDMETHODCALLTYPE test_SetMaterial(IDirect3DDevice9 *self, const D3DMATERIAL9 *material)
{assert(self==&device && expected.method==49);++calls;assert(!expected.data_bytes || (material && !memcmp(material,expected.data.bytes,expected.data_bytes)));return result(49);}
static HRESULT STDMETHODCALLTYPE test_SetLight(IDirect3DDevice9 *self, DWORD index, const D3DLIGHT9 *light)
{assert(self==&device && expected.method==51);++calls;assert((uint32_t)index==expected.args[0]);assert(!expected.data_bytes || (light && !memcmp(light,expected.data.bytes,expected.data_bytes)));return result(51);}
static HRESULT STDMETHODCALLTYPE test_LightEnable(IDirect3DDevice9 *self, DWORD Index, BOOL Enable)
{assert(self==&device && expected.method==53);++calls;assert((uint32_t)Index==expected.args[0]);assert((uint32_t)Enable==expected.args[1]);return result(53);}
static HRESULT STDMETHODCALLTYPE test_SetClipPlane(IDirect3DDevice9 *self, DWORD index, const float *plane)
{assert(self==&device && expected.method==55);++calls;assert((uint32_t)index==expected.args[0]);assert(!expected.data_bytes || (plane && !memcmp(plane,expected.data.bytes,expected.data_bytes)));return result(55);}
static HRESULT STDMETHODCALLTYPE test_SetRenderState(IDirect3DDevice9 *self, D3DRENDERSTATETYPE State, DWORD Value)
{assert(self==&device && expected.method==57);++calls;assert((uint32_t)State==expected.args[0]);assert((uint32_t)Value==expected.args[1]);return result(57);}
static HRESULT STDMETHODCALLTYPE test_SetClipStatus(IDirect3DDevice9 *self, const D3DCLIPSTATUS9 *clip_status)
{assert(self==&device && expected.method==62);++calls;assert(!expected.data_bytes || (clip_status && !memcmp(clip_status,expected.data.bytes,expected.data_bytes)));return result(62);}
static HRESULT STDMETHODCALLTYPE test_SetTexture(IDirect3DDevice9 *self, DWORD Stage, IDirect3DBaseTexture9* pTexture)
{assert(self==&device && expected.method==65);++calls;assert((uint32_t)Stage==expected.args[0]);assert((void *)pTexture==(null_binding?NULL:(void *)&object));return result(65);}
static HRESULT STDMETHODCALLTYPE test_SetTextureStageState(IDirect3DDevice9 *self, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
{assert(self==&device && expected.method==67);++calls;assert((uint32_t)Stage==expected.args[0]);assert((uint32_t)Type==expected.args[1]);assert((uint32_t)Value==expected.args[2]);return result(67);}
static HRESULT STDMETHODCALLTYPE test_SetSamplerState(IDirect3DDevice9 *self, DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value)
{assert(self==&device && expected.method==69);++calls;assert((uint32_t)Sampler==expected.args[0]);assert((uint32_t)Type==expected.args[1]);assert((uint32_t)Value==expected.args[2]);return result(69);}
static HRESULT STDMETHODCALLTYPE test_SetPaletteEntries(IDirect3DDevice9 *self, UINT palette_idx, const PALETTEENTRY *entries)
{assert(self==&device && expected.method==71);++calls;assert((uint32_t)palette_idx==expected.args[0]);assert(!expected.data_bytes || (entries && !memcmp(entries,expected.data.bytes,expected.data_bytes)));return result(71);}
static HRESULT STDMETHODCALLTYPE test_SetCurrentTexturePalette(IDirect3DDevice9 *self, UINT PaletteNumber)
{assert(self==&device && expected.method==73);++calls;assert((uint32_t)PaletteNumber==expected.args[0]);return result(73);}
static HRESULT STDMETHODCALLTYPE test_SetScissorRect(IDirect3DDevice9 *self, const RECT *rect)
{assert(self==&device && expected.method==75);++calls;assert(!expected.data_bytes || (rect && !memcmp(rect,expected.data.bytes,expected.data_bytes)));return result(75);}
static HRESULT STDMETHODCALLTYPE test_SetSoftwareVertexProcessing(IDirect3DDevice9 *self, BOOL bSoftware)
{assert(self==&device && expected.method==77);++calls;assert((uint32_t)bSoftware==expected.args[0]);return result(77);}
static HRESULT STDMETHODCALLTYPE test_SetNPatchMode(IDirect3DDevice9 *self, float nSegments)
{assert(self==&device && expected.method==79);++calls;assert(float_bits(nSegments)==expected.args[0]);return result(79);}
static HRESULT STDMETHODCALLTYPE test_DrawPrimitive(IDirect3DDevice9 *self, D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount)
{assert(self==&device && expected.method==81);++calls;assert((uint32_t)PrimitiveType==expected.args[0]);assert((uint32_t)StartVertex==expected.args[1]);assert((uint32_t)PrimitiveCount==expected.args[2]);return result(81);}
static HRESULT STDMETHODCALLTYPE test_DrawIndexedPrimitive(IDirect3DDevice9 *self, D3DPRIMITIVETYPE p0, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount)
{assert(self==&device && expected.method==82);++calls;assert((uint32_t)p0==expected.args[0]);assert((uint32_t)BaseVertexIndex==expected.args[1]);assert((uint32_t)MinVertexIndex==expected.args[2]);assert((uint32_t)NumVertices==expected.args[3]);assert((uint32_t)startIndex==expected.args[4]);assert((uint32_t)primCount==expected.args[5]);return result(82);}
static HRESULT STDMETHODCALLTYPE test_SetVertexDeclaration(IDirect3DDevice9 *self, IDirect3DVertexDeclaration9* pDecl)
{assert(self==&device && expected.method==87);++calls;assert((void *)pDecl==(null_binding?NULL:(void *)&object));return result(87);}
static HRESULT STDMETHODCALLTYPE test_SetFVF(IDirect3DDevice9 *self, DWORD FVF)
{assert(self==&device && expected.method==89);++calls;assert((uint32_t)FVF==expected.args[0]);return result(89);}
static HRESULT STDMETHODCALLTYPE test_SetVertexShader(IDirect3DDevice9 *self, IDirect3DVertexShader9* pShader)
{assert(self==&device && expected.method==92);++calls;assert((void *)pShader==(null_binding?NULL:(void *)&object));return result(92);}
static HRESULT STDMETHODCALLTYPE test_SetVertexShaderConstantF(IDirect3DDevice9 *self, UINT reg_idx, const float *data, UINT count)
{assert(self==&device && expected.method==94);++calls;assert((uint32_t)reg_idx==expected.args[0]);assert(!expected.data_bytes || (data && !memcmp(data,expected.data.bytes,expected.data_bytes)));assert((uint32_t)count==expected.args[1]);return result(94);}
static HRESULT STDMETHODCALLTYPE test_SetVertexShaderConstantI(IDirect3DDevice9 *self, UINT reg_idx, const int *data, UINT count)
{assert(self==&device && expected.method==96);++calls;assert((uint32_t)reg_idx==expected.args[0]);assert(!expected.data_bytes || (data && !memcmp(data,expected.data.bytes,expected.data_bytes)));assert((uint32_t)count==expected.args[1]);return result(96);}
static HRESULT STDMETHODCALLTYPE test_SetVertexShaderConstantB(IDirect3DDevice9 *self, UINT reg_idx, const BOOL *data, UINT count)
{assert(self==&device && expected.method==98);++calls;assert((uint32_t)reg_idx==expected.args[0]);assert(!expected.data_bytes || (data && !memcmp(data,expected.data.bytes,expected.data_bytes)));assert((uint32_t)count==expected.args[1]);return result(98);}
static HRESULT STDMETHODCALLTYPE test_SetStreamSource(IDirect3DDevice9 *self, UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData, UINT OffsetInBytes, UINT Stride)
{assert(self==&device && expected.method==100);++calls;assert((uint32_t)StreamNumber==expected.args[0]);assert((void *)pStreamData==(null_binding?NULL:(void *)&object));assert((uint32_t)OffsetInBytes==expected.args[3]);assert((uint32_t)Stride==expected.args[4]);return result(100);}
static HRESULT STDMETHODCALLTYPE test_SetStreamSourceFreq(IDirect3DDevice9 *self, UINT StreamNumber, UINT Divider)
{assert(self==&device && expected.method==102);++calls;assert((uint32_t)StreamNumber==expected.args[0]);assert((uint32_t)Divider==expected.args[1]);return result(102);}
static HRESULT STDMETHODCALLTYPE test_SetIndices(IDirect3DDevice9 *self, IDirect3DIndexBuffer9* pIndexData)
{assert(self==&device && expected.method==104);++calls;assert((void *)pIndexData==(null_binding?NULL:(void *)&object));return result(104);}
static HRESULT STDMETHODCALLTYPE test_SetPixelShader(IDirect3DDevice9 *self, IDirect3DPixelShader9* pShader)
{assert(self==&device && expected.method==107);++calls;assert((void *)pShader==(null_binding?NULL:(void *)&object));return result(107);}
static HRESULT STDMETHODCALLTYPE test_SetPixelShaderConstantF(IDirect3DDevice9 *self, UINT reg_idx, const float *data, UINT count)
{assert(self==&device && expected.method==109);++calls;assert((uint32_t)reg_idx==expected.args[0]);assert(!expected.data_bytes || (data && !memcmp(data,expected.data.bytes,expected.data_bytes)));assert((uint32_t)count==expected.args[1]);return result(109);}
static HRESULT STDMETHODCALLTYPE test_SetPixelShaderConstantI(IDirect3DDevice9 *self, UINT reg_idx, const int *data, UINT count)
{assert(self==&device && expected.method==111);++calls;assert((uint32_t)reg_idx==expected.args[0]);assert(!expected.data_bytes || (data && !memcmp(data,expected.data.bytes,expected.data_bytes)));assert((uint32_t)count==expected.args[1]);return result(111);}
static HRESULT STDMETHODCALLTYPE test_SetPixelShaderConstantB(IDirect3DDevice9 *self, UINT reg_idx, const BOOL *data, UINT count)
{assert(self==&device && expected.method==113);++calls;assert((uint32_t)reg_idx==expected.args[0]);assert(!expected.data_bytes || (data && !memcmp(data,expected.data.bytes,expected.data_bytes)));assert((uint32_t)count==expected.args[1]);return result(113);}
static const IDirect3DDevice9Vtbl device_vtable={
.TestCooperativeLevel=test_TestCooperativeLevel,
.EvictManagedResources=test_EvictManagedResources,
.SetDialogBoxMode=test_SetDialogBoxMode,
.SetRenderTarget=test_SetRenderTarget,
.SetDepthStencilSurface=test_SetDepthStencilSurface,
.BeginScene=test_BeginScene,
.EndScene=test_EndScene,
.Clear=test_Clear,
.SetTransform=test_SetTransform,
.MultiplyTransform=test_MultiplyTransform,
.SetViewport=test_SetViewport,
.SetMaterial=test_SetMaterial,
.SetLight=test_SetLight,
.LightEnable=test_LightEnable,
.SetClipPlane=test_SetClipPlane,
.SetRenderState=test_SetRenderState,
.SetClipStatus=test_SetClipStatus,
.SetTexture=test_SetTexture,
.SetTextureStageState=test_SetTextureStageState,
.SetSamplerState=test_SetSamplerState,
.SetPaletteEntries=test_SetPaletteEntries,
.SetCurrentTexturePalette=test_SetCurrentTexturePalette,
.SetScissorRect=test_SetScissorRect,
.SetSoftwareVertexProcessing=test_SetSoftwareVertexProcessing,
.SetNPatchMode=test_SetNPatchMode,
.DrawPrimitive=test_DrawPrimitive,
.DrawIndexedPrimitive=test_DrawIndexedPrimitive,
.SetVertexDeclaration=test_SetVertexDeclaration,
.SetFVF=test_SetFVF,
.SetVertexShader=test_SetVertexShader,
.SetVertexShaderConstantF=test_SetVertexShaderConstantF,
.SetVertexShaderConstantI=test_SetVertexShaderConstantI,
.SetVertexShaderConstantB=test_SetVertexShaderConstantB,
.SetStreamSource=test_SetStreamSource,
.SetStreamSourceFreq=test_SetStreamSourceFreq,
.SetIndices=test_SetIndices,
.SetPixelShader=test_SetPixelShader,
.SetPixelShaderConstantF=test_SetPixelShaderConstantF,
.SetPixelShaderConstantI=test_SetPixelShaderConstantI,
.SetPixelShaderConstantB=test_SetPixelShaderConstantB,
};
static void exercise(unsigned method,unsigned large)
{
 const struct pw_d3d9_command_schema *s=pw_d3d9_command_schema(method);size_t bytes;unsigned i,before=calls;HRESULT hr;
 memset(&expected,0,sizeof(expected));expected.method=method;deny=0;null_binding=0;
 for(i=0;i<s->words;i++)expected.args[i]=31+i;
 for(i=0;i<s->words;i++){
  if(s->boolean_words&(1u<<i))expected.args[i]=1;
  if(s->object_words&(1u<<i)){expected.args[i]=17;expected.args[i+1]=23;}
 }
 if(s->shape==PW_D3D9_DATA_CLEAR)expected.args[0]=large?256:3;
 if(s->shape==PW_D3D9_DATA_VECTOR4 || s->shape==PW_D3D9_DATA_BOOL)expected.args[1]=large?(s->shape==PW_D3D9_DATA_BOOL?1024:256):3;
 assert(!pw_d3d9_command_data_bytes(method,expected.args,&bytes));expected.data_bytes=bytes;
 for(i=0;i<bytes/4;i++)expected.data.words[i]=s->shape==PW_D3D9_DATA_BOOL?(i&1):0x3f800000u+i*0x10000u;
 if(method==82)expected.args[1]=0xffffffd6u;
 hr=pw_d3d9_native_command_dispatch(&device,&expected,acquire_object,&expected);
 assert(hr==result(method) && calls==before+1 && !held && acquires==releases);
 if(s->object_words){
  deny=1;before=calls;assert(pw_d3d9_native_command_dispatch(&device,&expected,acquire_object,&expected)==D3DERR_DEVICELOST);assert(calls==before && !held);--acquires;
  deny=0;null_binding=1;for(i=0;i<s->words;i++)if(s->object_words&(1u<<i))expected.args[i]=expected.args[i+1]=0;
  assert(pw_d3d9_native_command_dispatch(&device,&expected,NULL,NULL)==result(method));assert(!held && acquires==releases);
 }
 expected.data_bytes++;before=calls;assert(pw_d3d9_native_command_dispatch(&device,&expected,acquire_object,&expected)==D3DERR_INVALIDCALL);assert(calls==before);
}
int main(void)
{
 device.lpVtbl=&device_vtable;object.lpVtbl=&object_vtable;
#define EXERCISE(slot,name,words,shape,objects,booleans) exercise(slot,0);
 PW_D3D9_COMMAND_METHODS(EXERCISE)
#undef EXERCISE
 exercise(43,1);exercise(94,1);exercise(96,1);exercise(98,1);
 memset(&expected,0,sizeof(expected));expected.method=98;expected.args[1]=1;expected.data_bytes=4;expected.data.words[0]=2;
 assert(pw_d3d9_native_command_dispatch(&device,&expected,NULL,NULL)==D3DERR_INVALIDCALL);
 expected.method=84;assert(pw_d3d9_native_command_dispatch(&device,&expected,NULL,NULL)==E_NOTIMPL);
 assert(!held && acquires==8 && releases==8);
 puts("PASS native command ABI:40 methods, field mapping, signed/float bits, exact HRESULTs,8 typed pins, rejection and null bindings");return 0;
}
