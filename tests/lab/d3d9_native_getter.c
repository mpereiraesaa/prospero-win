/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_native_getter.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static IDirect3DDevice9 device;
static struct pw_d3d9_getter_request expected;
static unsigned calls,fail;
static size_t bytes;
static uint32_t data[1024];
static HRESULT status(void){return fail?(HRESULT)0x8876086c:(HRESULT)0x1234;}
static UINT STDMETHODCALLTYPE test_GetAvailableTextureMem(IDirect3DDevice9 *self)
{assert(self==&device && expected.method==4);++calls;UINT value;memcpy(&value,data,4);return value;}
static HRESULT STDMETHODCALLTYPE test_GetDisplayMode(IDirect3DDevice9 *self, UINT iSwapChain, D3DDISPLAYMODE* pMode)
{assert(self==&device && expected.method==8);++calls;assert((uint32_t)iSwapChain==expected.args[0]);assert(pMode);if(bytes)memcpy(pMode,data,bytes);return status();}
static UINT STDMETHODCALLTYPE test_GetNumberOfSwapChains(IDirect3DDevice9 *self)
{assert(self==&device && expected.method==15);++calls;UINT value;memcpy(&value,data,4);return value;}
static HRESULT STDMETHODCALLTYPE test_GetRasterStatus(IDirect3DDevice9 *self, UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus)
{assert(self==&device && expected.method==19);++calls;assert((uint32_t)iSwapChain==expected.args[0]);assert(pRasterStatus);if(bytes)memcpy(pRasterStatus,data,bytes);return status();}
static HRESULT STDMETHODCALLTYPE test_GetTransform(IDirect3DDevice9 *self, D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix)
{assert(self==&device && expected.method==45);++calls;assert((uint32_t)State==expected.args[0]);assert(pMatrix);if(bytes)memcpy(pMatrix,data,bytes);return status();}
static HRESULT STDMETHODCALLTYPE test_GetViewport(IDirect3DDevice9 *self, D3DVIEWPORT9* pViewport)
{assert(self==&device && expected.method==48);++calls;assert(pViewport);if(bytes)memcpy(pViewport,data,bytes);return status();}
static HRESULT STDMETHODCALLTYPE test_GetMaterial(IDirect3DDevice9 *self, D3DMATERIAL9* pMaterial)
{assert(self==&device && expected.method==50);++calls;assert(pMaterial);if(bytes)memcpy(pMaterial,data,bytes);return status();}
static HRESULT STDMETHODCALLTYPE test_GetLight(IDirect3DDevice9 *self, DWORD Index, D3DLIGHT9* output1)
{assert(self==&device && expected.method==52);++calls;assert((uint32_t)Index==expected.args[0]);assert(output1);if(bytes)memcpy(output1,data,bytes);return status();}
static HRESULT STDMETHODCALLTYPE test_GetLightEnable(IDirect3DDevice9 *self, DWORD Index, BOOL* pEnable)
{assert(self==&device && expected.method==54);++calls;assert((uint32_t)Index==expected.args[0]);assert(pEnable);if(bytes)memcpy(pEnable,data,bytes);return status();}
static HRESULT STDMETHODCALLTYPE test_GetClipPlane(IDirect3DDevice9 *self, DWORD Index, float* pPlane)
{assert(self==&device && expected.method==56);++calls;assert((uint32_t)Index==expected.args[0]);assert(pPlane);if(bytes)memcpy(pPlane,data,bytes);return status();}
static HRESULT STDMETHODCALLTYPE test_GetRenderState(IDirect3DDevice9 *self, D3DRENDERSTATETYPE State, DWORD* pValue)
{assert(self==&device && expected.method==58);++calls;assert((uint32_t)State==expected.args[0]);assert(pValue);if(bytes)memcpy(pValue,data,bytes);return status();}
static HRESULT STDMETHODCALLTYPE test_GetClipStatus(IDirect3DDevice9 *self, D3DCLIPSTATUS9* pClipStatus)
{assert(self==&device && expected.method==63);++calls;assert(pClipStatus);if(bytes)memcpy(pClipStatus,data,bytes);return status();}
static HRESULT STDMETHODCALLTYPE test_GetTextureStageState(IDirect3DDevice9 *self, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue)
{assert(self==&device && expected.method==66);++calls;assert((uint32_t)Stage==expected.args[0]);assert((uint32_t)Type==expected.args[1]);assert(pValue);if(bytes)memcpy(pValue,data,bytes);return status();}
static HRESULT STDMETHODCALLTYPE test_GetSamplerState(IDirect3DDevice9 *self, DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue)
{assert(self==&device && expected.method==68);++calls;assert((uint32_t)Sampler==expected.args[0]);assert((uint32_t)Type==expected.args[1]);assert(pValue);if(bytes)memcpy(pValue,data,bytes);return status();}
static HRESULT STDMETHODCALLTYPE test_ValidateDevice(IDirect3DDevice9 *self, DWORD* pNumPasses)
{assert(self==&device && expected.method==70);++calls;assert(pNumPasses);if(bytes)memcpy(pNumPasses,data,bytes);return status();}
static HRESULT STDMETHODCALLTYPE test_GetPaletteEntries(IDirect3DDevice9 *self, UINT PaletteNumber, PALETTEENTRY* pEntries)
{assert(self==&device && expected.method==72);++calls;assert((uint32_t)PaletteNumber==expected.args[0]);assert(pEntries);if(bytes)memcpy(pEntries,data,bytes);return status();}
static HRESULT STDMETHODCALLTYPE test_GetCurrentTexturePalette(IDirect3DDevice9 *self, UINT *PaletteNumber)
{assert(self==&device && expected.method==74);++calls;assert(PaletteNumber);if(bytes)memcpy(PaletteNumber,data,bytes);return status();}
static HRESULT STDMETHODCALLTYPE test_GetScissorRect(IDirect3DDevice9 *self, RECT* pRect)
{assert(self==&device && expected.method==76);++calls;assert(pRect);if(bytes)memcpy(pRect,data,bytes);return status();}
static BOOL STDMETHODCALLTYPE test_GetSoftwareVertexProcessing(IDirect3DDevice9 *self)
{assert(self==&device && expected.method==78);++calls;BOOL value;memcpy(&value,data,4);return value;}
static float STDMETHODCALLTYPE test_GetNPatchMode(IDirect3DDevice9 *self)
{assert(self==&device && expected.method==80);++calls;float value;memcpy(&value,data,4);return value;}
static HRESULT STDMETHODCALLTYPE test_GetFVF(IDirect3DDevice9 *self, DWORD* pFVF)
{assert(self==&device && expected.method==90);++calls;assert(pFVF);if(bytes)memcpy(pFVF,data,bytes);return status();}
static HRESULT STDMETHODCALLTYPE test_GetVertexShaderConstantF(IDirect3DDevice9 *self, UINT StartRegister, float* pConstantData, UINT Vector4fCount)
{assert(self==&device && expected.method==95);++calls;assert((uint32_t)StartRegister==expected.args[0]);assert(pConstantData);if(bytes)memcpy(pConstantData,data,bytes);assert((uint32_t)Vector4fCount==expected.args[1]);return status();}
static HRESULT STDMETHODCALLTYPE test_GetVertexShaderConstantI(IDirect3DDevice9 *self, UINT StartRegister, int* pConstantData, UINT Vector4iCount)
{assert(self==&device && expected.method==97);++calls;assert((uint32_t)StartRegister==expected.args[0]);assert(pConstantData);if(bytes)memcpy(pConstantData,data,bytes);assert((uint32_t)Vector4iCount==expected.args[1]);return status();}
static HRESULT STDMETHODCALLTYPE test_GetVertexShaderConstantB(IDirect3DDevice9 *self, UINT StartRegister, BOOL* pConstantData, UINT BoolCount)
{assert(self==&device && expected.method==99);++calls;assert((uint32_t)StartRegister==expected.args[0]);assert(pConstantData);if(bytes)memcpy(pConstantData,data,bytes);assert((uint32_t)BoolCount==expected.args[1]);return status();}
static HRESULT STDMETHODCALLTYPE test_GetStreamSourceFreq(IDirect3DDevice9 *self, UINT StreamNumber, UINT* Divider)
{assert(self==&device && expected.method==103);++calls;assert((uint32_t)StreamNumber==expected.args[0]);assert(Divider);if(bytes)memcpy(Divider,data,bytes);return status();}
static HRESULT STDMETHODCALLTYPE test_GetPixelShaderConstantF(IDirect3DDevice9 *self, UINT StartRegister, float* pConstantData, UINT Vector4fCount)
{assert(self==&device && expected.method==110);++calls;assert((uint32_t)StartRegister==expected.args[0]);assert(pConstantData);if(bytes)memcpy(pConstantData,data,bytes);assert((uint32_t)Vector4fCount==expected.args[1]);return status();}
static HRESULT STDMETHODCALLTYPE test_GetPixelShaderConstantI(IDirect3DDevice9 *self, UINT StartRegister, int* pConstantData, UINT Vector4iCount)
{assert(self==&device && expected.method==112);++calls;assert((uint32_t)StartRegister==expected.args[0]);assert(pConstantData);if(bytes)memcpy(pConstantData,data,bytes);assert((uint32_t)Vector4iCount==expected.args[1]);return status();}
static HRESULT STDMETHODCALLTYPE test_GetPixelShaderConstantB(IDirect3DDevice9 *self, UINT StartRegister, BOOL* pConstantData, UINT BoolCount)
{assert(self==&device && expected.method==114);++calls;assert((uint32_t)StartRegister==expected.args[0]);assert(pConstantData);if(bytes)memcpy(pConstantData,data,bytes);assert((uint32_t)BoolCount==expected.args[1]);return status();}
static const IDirect3DDevice9Vtbl vtable={
.GetAvailableTextureMem=test_GetAvailableTextureMem,
.GetDisplayMode=test_GetDisplayMode,
.GetNumberOfSwapChains=test_GetNumberOfSwapChains,
.GetRasterStatus=test_GetRasterStatus,
.GetTransform=test_GetTransform,
.GetViewport=test_GetViewport,
.GetMaterial=test_GetMaterial,
.GetLight=test_GetLight,
.GetLightEnable=test_GetLightEnable,
.GetClipPlane=test_GetClipPlane,
.GetRenderState=test_GetRenderState,
.GetClipStatus=test_GetClipStatus,
.GetTextureStageState=test_GetTextureStageState,
.GetSamplerState=test_GetSamplerState,
.ValidateDevice=test_ValidateDevice,
.GetPaletteEntries=test_GetPaletteEntries,
.GetCurrentTexturePalette=test_GetCurrentTexturePalette,
.GetScissorRect=test_GetScissorRect,
.GetSoftwareVertexProcessing=test_GetSoftwareVertexProcessing,
.GetNPatchMode=test_GetNPatchMode,
.GetFVF=test_GetFVF,
.GetVertexShaderConstantF=test_GetVertexShaderConstantF,
.GetVertexShaderConstantI=test_GetVertexShaderConstantI,
.GetVertexShaderConstantB=test_GetVertexShaderConstantB,
.GetStreamSourceFreq=test_GetStreamSourceFreq,
.GetPixelShaderConstantF=test_GetPixelShaderConstantF,
.GetPixelShaderConstantI=test_GetPixelShaderConstantI,
.GetPixelShaderConstantB=test_GetPixelShaderConstantB,
};
static void exercise(unsigned method,unsigned count)
{
 const struct pw_d3d9_getter_schema *schema=pw_d3d9_getter_schema(method);struct pw_d3d9_getter_reply reply;
 unsigned i,before;int value=method==4 || method==15 || method==78 || method==80;
 memset(&expected,0,sizeof(expected));expected.method=method;
 for(i=0;i<schema->args;i++)expected.args[i]=31+i;
 if(schema->element)expected.args[1]=count;
 assert(!pw_d3d9_getter_bytes(&expected,&bytes));
 for(i=0;i<1024;i++)data[i]=0x80000042u+i*0x10203u;
 if(method==80)data[0]=0x7fc12345; /* preserve a quiet NaN payload */
 for(fail=0;fail<2;fail++){
  memset(&reply,0xcc,sizeof(reply));before=calls;
  assert(!pw_d3d9_native_getter_dispatch(&device,&expected,&reply));assert(calls==before+1 && reply.method==method);
  if(!value && fail){assert(reply.hresult==0x8876086c && !reply.bytes);for(i=0;i<sizeof(reply.data.bytes);i++)assert(!reply.data.bytes[i]);}
  else {assert(reply.hresult==(value?0:0x1234) && reply.bytes==bytes);assert(!memcmp(reply.data.bytes,data,bytes));}
 }
}
int main(void)
{
 struct pw_d3d9_getter_reply reply,original;unsigned before;
 device.lpVtbl=&vtable;
#define EXERCISE(slot,name,args,bytes,element) exercise(slot,3);
 PW_D3D9_GETTER_METHODS(EXERCISE)
#undef EXERCISE
 exercise(95,256);exercise(97,256);exercise(99,1024);exercise(110,0);exercise(112,0);exercise(114,0);
 memset(&reply,0xcc,sizeof(reply));original=reply;before=calls;expected.method=95;expected.args[0]=0;expected.args[1]=UINT32_MAX;
 assert(pw_d3d9_native_getter_dispatch(&device,&expected,&reply)==PW_D3D9_GETTER_UNSUPPORTED);
 expected.method=48;expected.args[0]=1;expected.args[1]=0;
 assert(pw_d3d9_native_getter_dispatch(&device,&expected,&reply)==PW_D3D9_GETTER_INVALID);
 expected.method=84;assert(pw_d3d9_native_getter_dispatch(&device,&expected,&reply)==PW_D3D9_GETTER_UNSUPPORTED);
 assert(calls==before && !memcmp(&reply,&original,sizeof(reply)));
 puts("PASS native getter ABI:28 methods, exact HRESULT/BOOL/float bits, atomic failures, maximum and zero arrays");return 0;
}
