/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_device_methods.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static DWORD creation_flags=D3DCREATE_HARDWARE_VERTEXPROCESSING;
static HRESULT creation_result=S_OK;
static HRESULT WINAPI local_creation(IDirect3DDevice9 *d,D3DDEVICE_CREATION_PARAMETERS *p)
{(void)d;memset(p,0,sizeof(*p));p->BehaviorFlags=creation_flags;return creation_result;}
static unsigned calls,resolves,failures,mode;static LONG device_refs=1;
static ULONG WINAPI self_addref(IDirect3DDevice9 *d){(void)d;return ++device_refs;}
static ULONG WINAPI self_release(IDirect3DDevice9 *d){(void)d;return --device_refs;}static uint32_t method,args[6],expected_bytes;
static unsigned char payload[4096];static IDirect3DDevice9 *owner;static IUnknown *object=(IUnknown *)(uintptr_t)0x1234;
static unsigned expected_kind;
static HRESULT command(IDirect3DDevice9 *device,const struct pw_d3d9_command *c)
{
 unsigned char wire[PW_D3D9_COMMAND_MAX];size_t written;
 assert(device==owner && c->method==method && !memcmp(args,c->args,sizeof(args)));
 assert(c->data_bytes==expected_bytes && !memcmp(payload,c->data.bytes,expected_bytes));
 assert(!pw_d3d9_command_encode(wire,sizeof(wire),&written,c));calls++;return 0x1234;
}
static HRESULT getter(IDirect3DDevice9 *device,const struct pw_d3d9_getter_request *q,struct pw_d3d9_getter_reply *r)
{
 size_t bytes;assert(device_refs>=2);assert(device==owner && q->method==method && !memcmp(args,q->args,sizeof(q->args)));
 assert(!pw_d3d9_getter_bytes(q,&bytes));calls++;
 r->method=q->method;r->hresult=0x1234;r->bytes=bytes;memcpy(r->data.bytes,payload,bytes);
 if(mode==1)return D3DERR_INVALIDCALL;
 if(mode==2)r->method++;
 if(mode==3)r->bytes++;
 if(mode==4)r->hresult++;
 if(mode==5){IDirect3DDevice9_Release(device);assert(device_refs==1);}
 return 0x1234;
}
static HRESULT resolve(IDirect3DDevice9 *device,IUnknown *local,uint32_t kind,struct pw_d3d9_object_ref *ref)
{
 assert(device==owner && local==object && kind==expected_kind);resolves++;
 if(mode==5)return D3DERR_INVALIDCALL;
 *ref=(struct pw_d3d9_object_ref){77,88};return S_OK;
}
static void fail(IDirect3DDevice9 *device,HRESULT hr){assert(device==owner && hr==E_FAIL);failures++;}
static void setup(unsigned slot){method=slot;memset(args,0,sizeof(args));expected_bytes=0;memset(payload,0xa5,sizeof(payload));}
static HRESULT invoke_constant(IDirect3DDevice9 *d,unsigned slot,UINT start,const void *data,UINT count)
{
 switch(slot){
 case 94:return IDirect3DDevice9_SetVertexShaderConstantF(d,start,data,count);
 case 96:return IDirect3DDevice9_SetVertexShaderConstantI(d,start,data,count);
 case 98:return IDirect3DDevice9_SetVertexShaderConstantB(d,start,data,count);
 case 109:return IDirect3DDevice9_SetPixelShaderConstantF(d,start,data,count);
 case 111:return IDirect3DDevice9_SetPixelShaderConstantI(d,start,data,count);
 default:return IDirect3DDevice9_SetPixelShaderConstantB(d,start,data,count);
 }
}
int main(void)
{
 IDirect3DDevice9Vtbl table={0};IDirect3DDevice9 device={&table};
 struct pw_d3d9_device_methods_ops callbacks={command,getter,fail,resolve};owner=&device;
 table.AddRef=self_addref;table.Release=self_release;table.GetCreationParameters=local_creation;pw_d3d9_device_methods_install(&table,&callbacks);
 assert(!table.QueryInterface && !table.Reset && !table.Present && !table.CreateTexture);
 {setup(3);
 assert(IDirect3DDevice9_TestCooperativeLevel(&device)==0x1234);
 }
 {setup(5);
 assert(IDirect3DDevice9_EvictManagedResources(&device)==0x1234);
 }
 {setup(20);
 args[0]=1;
 assert(IDirect3DDevice9_SetDialogBoxMode(&device,-3)==0x1234);
 }
 {setup(37);
 expected_kind=PW_D3D9_KIND_SURFACE;args[1]=77;args[2]=88;
 args[0]=(uint32_t)2;
 assert(IDirect3DDevice9_SetRenderTarget(&device,2,(IDirect3DSurface9*)object)==0x1234);
 mode=5;
 assert(IDirect3DDevice9_SetRenderTarget(&device,2,(IDirect3DSurface9*)object)==D3DERR_INVALIDCALL);mode=0;
 args[1]=args[2]=0;
 assert(IDirect3DDevice9_SetRenderTarget(&device,2,NULL)==0x1234);
 }
 {setup(39);
 expected_kind=PW_D3D9_KIND_SURFACE;args[0]=77;args[1]=88;
 assert(IDirect3DDevice9_SetDepthStencilSurface(&device,(IDirect3DSurface9*)object)==0x1234);
 mode=5;
 assert(IDirect3DDevice9_SetDepthStencilSurface(&device,(IDirect3DSurface9*)object)==D3DERR_INVALIDCALL);mode=0;
 args[0]=args[1]=0;
 assert(IDirect3DDevice9_SetDepthStencilSurface(&device,NULL)==0x1234);
 }
 {setup(41);
 assert(IDirect3DDevice9_BeginScene(&device)==0x1234);
 }
 {setup(42);
 assert(IDirect3DDevice9_EndScene(&device)==0x1234);
 }
 {setup(43);
 D3DRECT v1[256];memset(v1,0xa5,sizeof(v1));
 args[0]=2;args[1]=2;args[2]=2;args[3]=0x3f000000;args[4]=2;expected_bytes=32;
 assert(IDirect3DDevice9_Clear(&device,2,v1,2,2,0.5f,2)==0x1234);
 }
 {setup(44);
 D3DMATRIX v1[1];memset(v1,0xa5,sizeof(v1));
 expected_bytes=64;
 args[0]=2;
 assert(IDirect3DDevice9_SetTransform(&device,2,v1)==0x1234);
 memset(payload,0,64);for(unsigned i=0;i<4;i++){uint32_t one=0x3f800000;memcpy(payload+20*i,&one,4);}
 assert(IDirect3DDevice9_SetTransform(&device,2,NULL)==0x1234);
 }
 {setup(46);
 D3DMATRIX v1[1];memset(v1,0xa5,sizeof(v1));
 expected_bytes=64;
 args[0]=2;
 assert(IDirect3DDevice9_MultiplyTransform(&device,2,v1)==0x1234);
 assert(IDirect3DDevice9_MultiplyTransform(&device,2,NULL)==D3DERR_INVALIDCALL);
 }
 {setup(47);
 D3DVIEWPORT9 v0[1];memset(v0,0xa5,sizeof(v0));
 expected_bytes=24;
 assert(IDirect3DDevice9_SetViewport(&device,v0)==0x1234);
 assert(IDirect3DDevice9_SetViewport(&device,NULL)==D3DERR_INVALIDCALL);
 }
 {setup(49);
 D3DMATERIAL9 v0[1];memset(v0,0xa5,sizeof(v0));
 expected_bytes=68;
 assert(IDirect3DDevice9_SetMaterial(&device,v0)==0x1234);
 assert(IDirect3DDevice9_SetMaterial(&device,NULL)==D3DERR_INVALIDCALL);
 }
 {setup(51);
 D3DLIGHT9 v1[1];memset(v1,0xa5,sizeof(v1));
 expected_bytes=104;
 args[0]=2;
 assert(IDirect3DDevice9_SetLight(&device,2,v1)==0x1234);
 assert(IDirect3DDevice9_SetLight(&device,2,NULL)==D3DERR_INVALIDCALL);
 }
 {setup(53);
 args[0]=(uint32_t)2;
 args[1]=1;
 assert(IDirect3DDevice9_LightEnable(&device,2,-3)==0x1234);
 }
 {setup(55);
 float v1[4];memset(v1,0xa5,sizeof(v1));
 expected_bytes=16;
 args[0]=2;
 assert(IDirect3DDevice9_SetClipPlane(&device,2,v1)==0x1234);
 assert(IDirect3DDevice9_SetClipPlane(&device,2,NULL)==D3DERR_INVALIDCALL);
 }
 {setup(57);
 args[0]=(uint32_t)2;
 args[1]=(uint32_t)2;
 assert(IDirect3DDevice9_SetRenderState(&device,2,2)==0x1234);
 }
 {setup(62);
 D3DCLIPSTATUS9 v0[1];memset(v0,0xa5,sizeof(v0));
 expected_bytes=8;
 assert(IDirect3DDevice9_SetClipStatus(&device,v0)==0x1234);
 assert(IDirect3DDevice9_SetClipStatus(&device,NULL)==D3DERR_INVALIDCALL);
 }
 {setup(65);
 expected_kind=PW_D3D9_KIND_TEXTURE_2D;args[1]=77;args[2]=88;
 args[0]=(uint32_t)2;
 assert(IDirect3DDevice9_SetTexture(&device,2,(IDirect3DBaseTexture9*)object)==0x1234);
 mode=5;
 assert(IDirect3DDevice9_SetTexture(&device,2,(IDirect3DBaseTexture9*)object)==D3DERR_INVALIDCALL);mode=0;
 args[1]=args[2]=0;
 assert(IDirect3DDevice9_SetTexture(&device,2,NULL)==0x1234);
 }
 {setup(67);
 args[0]=(uint32_t)2;
 args[1]=(uint32_t)2;
 args[2]=(uint32_t)2;
 assert(IDirect3DDevice9_SetTextureStageState(&device,2,2,2)==0x1234);
 }
 {setup(69);
 args[0]=(uint32_t)2;
 args[1]=(uint32_t)2;
 args[2]=(uint32_t)2;
 assert(IDirect3DDevice9_SetSamplerState(&device,2,2,2)==0x1234);
 }
 {setup(71);
 PALETTEENTRY v1[256];memset(v1,0xa5,sizeof(v1));
 expected_bytes=1024;
 args[0]=2;
 assert(IDirect3DDevice9_SetPaletteEntries(&device,2,v1)==0x1234);
 assert(IDirect3DDevice9_SetPaletteEntries(&device,2,NULL)==D3DERR_INVALIDCALL);
 }
 {setup(73);
 args[0]=(uint32_t)2;
 assert(IDirect3DDevice9_SetCurrentTexturePalette(&device,2)==0x1234);
 }
 {setup(75);
 RECT v0[1];memset(v0,0xa5,sizeof(v0));
 expected_bytes=16;
 assert(IDirect3DDevice9_SetScissorRect(&device,v0)==0x1234);
 assert(IDirect3DDevice9_SetScissorRect(&device,NULL)==D3DERR_INVALIDCALL);
 }
 {setup(77);
 args[0]=1;
 assert(IDirect3DDevice9_SetSoftwareVertexProcessing(&device,-3)==0x1234);
 }
 {setup(79);
 args[0]=0x3f000000;
 assert(IDirect3DDevice9_SetNPatchMode(&device,0.5f)==0x1234);
 }
 {setup(81);
 args[0]=(uint32_t)2;
 args[1]=(uint32_t)2;
 args[2]=(uint32_t)2;
 assert(IDirect3DDevice9_DrawPrimitive(&device,2,2,2)==0x1234);
 }
 {setup(82);
 args[0]=(uint32_t)2;
 args[1]=(uint32_t)-3;
 args[2]=(uint32_t)2;
 args[3]=(uint32_t)2;
 args[4]=(uint32_t)2;
 args[5]=(uint32_t)2;
 assert(IDirect3DDevice9_DrawIndexedPrimitive(&device,2,-3,2,2,2,2)==0x1234);
 }
 {setup(87);
 expected_kind=PW_D3D9_KIND_VERTEX_DECLARATION;args[0]=77;args[1]=88;
 assert(IDirect3DDevice9_SetVertexDeclaration(&device,(IDirect3DVertexDeclaration9*)object)==0x1234);
 mode=5;
 assert(IDirect3DDevice9_SetVertexDeclaration(&device,(IDirect3DVertexDeclaration9*)object)==D3DERR_INVALIDCALL);mode=0;
 args[0]=args[1]=0;
 assert(IDirect3DDevice9_SetVertexDeclaration(&device,NULL)==0x1234);
 }
 {setup(89);
 args[0]=(uint32_t)2;
 assert(IDirect3DDevice9_SetFVF(&device,2)==0x1234);
 }
 {setup(92);
 expected_kind=PW_D3D9_KIND_VERTEX_SHADER;args[0]=77;args[1]=88;
 assert(IDirect3DDevice9_SetVertexShader(&device,(IDirect3DVertexShader9*)object)==0x1234);
 mode=5;
 assert(IDirect3DDevice9_SetVertexShader(&device,(IDirect3DVertexShader9*)object)==D3DERR_INVALIDCALL);mode=0;
 args[0]=args[1]=0;
 assert(IDirect3DDevice9_SetVertexShader(&device,NULL)==0x1234);
 }
 {setup(94);
 float v1[1024];memset(v1,0xa5,sizeof(v1));
 args[0]=2;args[1]=2;expected_bytes=32;
 assert(IDirect3DDevice9_SetVertexShaderConstantF(&device,2,v1,2)==0x1234);
 assert(IDirect3DDevice9_SetVertexShaderConstantF(&device,2,NULL,2)==D3DERR_INVALIDCALL);
 args[1]=0;expected_bytes=0;
 assert(IDirect3DDevice9_SetVertexShaderConstantF(&device,2,v1,0)==0x1234);
 assert(IDirect3DDevice9_SetVertexShaderConstantF(&device,2,v1,8193)==D3DERR_INVALIDCALL);
 }
 {setup(96);
 int v1[1024];memset(v1,0xa5,sizeof(v1));
 args[0]=2;args[1]=2;expected_bytes=32;
 assert(IDirect3DDevice9_SetVertexShaderConstantI(&device,2,v1,2)==0x1234);
 assert(IDirect3DDevice9_SetVertexShaderConstantI(&device,2,NULL,2)==D3DERR_INVALIDCALL);
 args[1]=0;expected_bytes=0;
 assert(IDirect3DDevice9_SetVertexShaderConstantI(&device,2,v1,0)==0x1234);
 assert(IDirect3DDevice9_SetVertexShaderConstantI(&device,2,v1,2049)==D3DERR_INVALIDCALL);
 }
 {setup(98);
 BOOL v1[1024];memset(v1,0xa5,sizeof(v1));
 args[0]=2;args[1]=2;expected_bytes=8;
 ((uint32_t *)payload)[0]=1;((uint32_t *)payload)[1]=1;
 assert(IDirect3DDevice9_SetVertexShaderConstantB(&device,2,v1,2)==0x1234);
 assert(IDirect3DDevice9_SetVertexShaderConstantB(&device,2,NULL,2)==D3DERR_INVALIDCALL);
 args[1]=0;expected_bytes=0;
 assert(IDirect3DDevice9_SetVertexShaderConstantB(&device,2,v1,0)==0x1234);
 assert(IDirect3DDevice9_SetVertexShaderConstantB(&device,2,v1,2049)==D3DERR_INVALIDCALL);
 }
 {setup(100);
 expected_kind=PW_D3D9_KIND_VERTEX_BUFFER;args[1]=77;args[2]=88;
 args[0]=(uint32_t)2;
 args[3]=(uint32_t)2;
 args[4]=(uint32_t)2;
 assert(IDirect3DDevice9_SetStreamSource(&device,2,(IDirect3DVertexBuffer9*)object,2,2)==0x1234);
 mode=5;
 assert(IDirect3DDevice9_SetStreamSource(&device,2,(IDirect3DVertexBuffer9*)object,2,2)==D3DERR_INVALIDCALL);mode=0;
 args[1]=args[2]=0;
 assert(IDirect3DDevice9_SetStreamSource(&device,2,NULL,2,2)==0x1234);
 }
 {setup(102);
 args[0]=(uint32_t)2;
 args[1]=(uint32_t)2;
 assert(IDirect3DDevice9_SetStreamSourceFreq(&device,2,2)==0x1234);
 }
 {setup(104);
 expected_kind=PW_D3D9_KIND_INDEX_BUFFER;args[0]=77;args[1]=88;
 assert(IDirect3DDevice9_SetIndices(&device,(IDirect3DIndexBuffer9*)object)==0x1234);
 mode=5;
 assert(IDirect3DDevice9_SetIndices(&device,(IDirect3DIndexBuffer9*)object)==D3DERR_INVALIDCALL);mode=0;
 args[0]=args[1]=0;
 assert(IDirect3DDevice9_SetIndices(&device,NULL)==0x1234);
 }
 {setup(107);
 expected_kind=PW_D3D9_KIND_PIXEL_SHADER;args[0]=77;args[1]=88;
 assert(IDirect3DDevice9_SetPixelShader(&device,(IDirect3DPixelShader9*)object)==0x1234);
 mode=5;
 assert(IDirect3DDevice9_SetPixelShader(&device,(IDirect3DPixelShader9*)object)==D3DERR_INVALIDCALL);mode=0;
 args[0]=args[1]=0;
 assert(IDirect3DDevice9_SetPixelShader(&device,NULL)==0x1234);
 }
 {setup(109);
 float v1[1024];memset(v1,0xa5,sizeof(v1));
 args[0]=2;args[1]=2;expected_bytes=32;
 assert(IDirect3DDevice9_SetPixelShaderConstantF(&device,2,v1,2)==0x1234);
 assert(IDirect3DDevice9_SetPixelShaderConstantF(&device,2,NULL,2)==D3DERR_INVALIDCALL);
 args[1]=0;expected_bytes=0;
 assert(IDirect3DDevice9_SetPixelShaderConstantF(&device,2,v1,0)==0x1234);
 assert(IDirect3DDevice9_SetPixelShaderConstantF(&device,2,v1,257)==D3DERR_INVALIDCALL);
 }
 {setup(111);
 int v1[1024];memset(v1,0xa5,sizeof(v1));
 args[0]=2;args[1]=2;expected_bytes=32;
 assert(IDirect3DDevice9_SetPixelShaderConstantI(&device,2,v1,2)==0x1234);
 assert(IDirect3DDevice9_SetPixelShaderConstantI(&device,2,NULL,2)==D3DERR_INVALIDCALL);
 args[1]=0;expected_bytes=0;
 assert(IDirect3DDevice9_SetPixelShaderConstantI(&device,2,v1,0)==0x1234);
 assert(IDirect3DDevice9_SetPixelShaderConstantI(&device,2,v1,257)==D3DERR_INVALIDCALL);
 }
 {setup(113);
 BOOL v1[1024];memset(v1,0xa5,sizeof(v1));
 args[0]=2;args[1]=2;expected_bytes=8;
 ((uint32_t *)payload)[0]=1;((uint32_t *)payload)[1]=1;
 assert(IDirect3DDevice9_SetPixelShaderConstantB(&device,2,v1,2)==0x1234);
 assert(IDirect3DDevice9_SetPixelShaderConstantB(&device,2,NULL,2)==D3DERR_INVALIDCALL);
 args[1]=0;expected_bytes=0;
 assert(IDirect3DDevice9_SetPixelShaderConstantB(&device,2,v1,0)==0x1234);
 assert(IDirect3DDevice9_SetPixelShaderConstantB(&device,2,v1,1025)==D3DERR_INVALIDCALL);
 }
 {setup(4);
 { UINT value=IDirect3DDevice9_GetAvailableTextureMem(&device);assert(!memcmp(&value,payload,4));}
 }
 {setup(8);
 D3DDISPLAYMODE output[1];memset(output,0xcc,sizeof(output));
 args[0]=2;
 assert(IDirect3DDevice9_GetDisplayMode(&device,2,output)==0x1234);
 {struct pw_d3d9_getter_request q={8,{0}};size_t bytes;memcpy(q.args,args,sizeof(q.args));assert(!pw_d3d9_getter_bytes(&q,&bytes));assert(!memcmp(output,payload,bytes));}
 assert(IDirect3DDevice9_GetDisplayMode(&device,2,NULL)==D3DERR_INVALIDCALL);
 }
 {setup(15);
 { UINT value=IDirect3DDevice9_GetNumberOfSwapChains(&device);assert(!memcmp(&value,payload,4));}
 }
 {setup(19);
 D3DRASTER_STATUS output[1];memset(output,0xcc,sizeof(output));
 args[0]=2;
 assert(IDirect3DDevice9_GetRasterStatus(&device,2,output)==0x1234);
 {struct pw_d3d9_getter_request q={19,{0}};size_t bytes;memcpy(q.args,args,sizeof(q.args));assert(!pw_d3d9_getter_bytes(&q,&bytes));assert(!memcmp(output,payload,bytes));}
 assert(IDirect3DDevice9_GetRasterStatus(&device,2,NULL)==D3DERR_INVALIDCALL);
 }
 {setup(45);
 D3DMATRIX output[1];memset(output,0xcc,sizeof(output));
 args[0]=2;
 assert(IDirect3DDevice9_GetTransform(&device,2,output)==0x1234);
 {struct pw_d3d9_getter_request q={45,{0}};size_t bytes;memcpy(q.args,args,sizeof(q.args));assert(!pw_d3d9_getter_bytes(&q,&bytes));assert(!memcmp(output,payload,bytes));}
 assert(IDirect3DDevice9_GetTransform(&device,2,NULL)==D3DERR_INVALIDCALL);
 }
 {setup(48);
 D3DVIEWPORT9 output[1];memset(output,0xcc,sizeof(output));
 assert(IDirect3DDevice9_GetViewport(&device,output)==0x1234);
 {struct pw_d3d9_getter_request q={48,{0}};size_t bytes;memcpy(q.args,args,sizeof(q.args));assert(!pw_d3d9_getter_bytes(&q,&bytes));assert(!memcmp(output,payload,bytes));}
 assert(IDirect3DDevice9_GetViewport(&device,NULL)==D3DERR_INVALIDCALL);
 }
 {setup(50);
 D3DMATERIAL9 output[1];memset(output,0xcc,sizeof(output));
 assert(IDirect3DDevice9_GetMaterial(&device,output)==0x1234);
 {struct pw_d3d9_getter_request q={50,{0}};size_t bytes;memcpy(q.args,args,sizeof(q.args));assert(!pw_d3d9_getter_bytes(&q,&bytes));assert(!memcmp(output,payload,bytes));}
 assert(IDirect3DDevice9_GetMaterial(&device,NULL)==D3DERR_INVALIDCALL);
 }
 {setup(52);
 D3DLIGHT9 output[1];memset(output,0xcc,sizeof(output));
 args[0]=2;
 assert(IDirect3DDevice9_GetLight(&device,2,output)==0x1234);
 {struct pw_d3d9_getter_request q={52,{0}};size_t bytes;memcpy(q.args,args,sizeof(q.args));assert(!pw_d3d9_getter_bytes(&q,&bytes));assert(!memcmp(output,payload,bytes));}
 assert(IDirect3DDevice9_GetLight(&device,2,NULL)==D3DERR_INVALIDCALL);
 }
 {setup(54);
 BOOL output[1];memset(output,0xcc,sizeof(output));
 args[0]=2;
 assert(IDirect3DDevice9_GetLightEnable(&device,2,output)==0x1234);
 {struct pw_d3d9_getter_request q={54,{0}};size_t bytes;memcpy(q.args,args,sizeof(q.args));assert(!pw_d3d9_getter_bytes(&q,&bytes));assert(!memcmp(output,payload,bytes));}
 assert(IDirect3DDevice9_GetLightEnable(&device,2,NULL)==D3DERR_INVALIDCALL);
 }
 {setup(56);
 float output[4];memset(output,0xcc,sizeof(output));
 args[0]=2;
 assert(IDirect3DDevice9_GetClipPlane(&device,2,output)==0x1234);
 {struct pw_d3d9_getter_request q={56,{0}};size_t bytes;memcpy(q.args,args,sizeof(q.args));assert(!pw_d3d9_getter_bytes(&q,&bytes));assert(!memcmp(output,payload,bytes));}
 assert(IDirect3DDevice9_GetClipPlane(&device,2,NULL)==D3DERR_INVALIDCALL);
 }
 {setup(58);
 DWORD output[1];memset(output,0xcc,sizeof(output));
 args[0]=2;
 assert(IDirect3DDevice9_GetRenderState(&device,2,output)==0x1234);
 {struct pw_d3d9_getter_request q={58,{0}};size_t bytes;memcpy(q.args,args,sizeof(q.args));assert(!pw_d3d9_getter_bytes(&q,&bytes));assert(!memcmp(output,payload,bytes));}
 assert(IDirect3DDevice9_GetRenderState(&device,2,NULL)==D3DERR_INVALIDCALL);
 }
 {setup(63);
 D3DCLIPSTATUS9 output[1];memset(output,0xcc,sizeof(output));
 assert(IDirect3DDevice9_GetClipStatus(&device,output)==0x1234);
 {struct pw_d3d9_getter_request q={63,{0}};size_t bytes;memcpy(q.args,args,sizeof(q.args));assert(!pw_d3d9_getter_bytes(&q,&bytes));assert(!memcmp(output,payload,bytes));}
 assert(IDirect3DDevice9_GetClipStatus(&device,NULL)==D3DERR_INVALIDCALL);
 }
 {setup(66);
 DWORD output[1];memset(output,0xcc,sizeof(output));
 args[0]=2;
 args[1]=2;
 assert(IDirect3DDevice9_GetTextureStageState(&device,2,2,output)==0x1234);
 {struct pw_d3d9_getter_request q={66,{0}};size_t bytes;memcpy(q.args,args,sizeof(q.args));assert(!pw_d3d9_getter_bytes(&q,&bytes));assert(!memcmp(output,payload,bytes));}
 assert(IDirect3DDevice9_GetTextureStageState(&device,2,2,NULL)==D3DERR_INVALIDCALL);
 }
 {setup(68);
 DWORD output[1];memset(output,0xcc,sizeof(output));
 args[0]=2;
 args[1]=2;
 assert(IDirect3DDevice9_GetSamplerState(&device,2,2,output)==0x1234);
 {struct pw_d3d9_getter_request q={68,{0}};size_t bytes;memcpy(q.args,args,sizeof(q.args));assert(!pw_d3d9_getter_bytes(&q,&bytes));assert(!memcmp(output,payload,bytes));}
 assert(IDirect3DDevice9_GetSamplerState(&device,2,2,NULL)==D3DERR_INVALIDCALL);
 }
 {setup(70);
 DWORD output[1];memset(output,0xcc,sizeof(output));
 assert(IDirect3DDevice9_ValidateDevice(&device,output)==0x1234);
 {struct pw_d3d9_getter_request q={70,{0}};size_t bytes;memcpy(q.args,args,sizeof(q.args));assert(!pw_d3d9_getter_bytes(&q,&bytes));assert(!memcmp(output,payload,bytes));}
 assert(IDirect3DDevice9_ValidateDevice(&device,NULL)==D3DERR_INVALIDCALL);
 }
 {setup(72);
 PALETTEENTRY output[256];memset(output,0xcc,sizeof(output));
 args[0]=2;
 assert(IDirect3DDevice9_GetPaletteEntries(&device,2,output)==0x1234);
 {struct pw_d3d9_getter_request q={72,{0}};size_t bytes;memcpy(q.args,args,sizeof(q.args));assert(!pw_d3d9_getter_bytes(&q,&bytes));assert(!memcmp(output,payload,bytes));}
 assert(IDirect3DDevice9_GetPaletteEntries(&device,2,NULL)==D3DERR_INVALIDCALL);
 }
 {setup(74);
 UINT output[1];memset(output,0xcc,sizeof(output));
 assert(IDirect3DDevice9_GetCurrentTexturePalette(&device,output)==0x1234);
 {struct pw_d3d9_getter_request q={74,{0}};size_t bytes;memcpy(q.args,args,sizeof(q.args));assert(!pw_d3d9_getter_bytes(&q,&bytes));assert(!memcmp(output,payload,bytes));}
 assert(IDirect3DDevice9_GetCurrentTexturePalette(&device,NULL)==D3DERR_INVALIDCALL);
 }
 {setup(76);
 RECT output[1];memset(output,0xcc,sizeof(output));
 assert(IDirect3DDevice9_GetScissorRect(&device,output)==0x1234);
 {struct pw_d3d9_getter_request q={76,{0}};size_t bytes;memcpy(q.args,args,sizeof(q.args));assert(!pw_d3d9_getter_bytes(&q,&bytes));assert(!memcmp(output,payload,bytes));}
 assert(IDirect3DDevice9_GetScissorRect(&device,NULL)==D3DERR_INVALIDCALL);
 }
 {setup(78);
 { BOOL value=IDirect3DDevice9_GetSoftwareVertexProcessing(&device);assert(!memcmp(&value,payload,4));}
 }
 {setup(80);
 { float value=IDirect3DDevice9_GetNPatchMode(&device);assert(!memcmp(&value,payload,4));}
 }
 {setup(90);
 DWORD output[1];memset(output,0xcc,sizeof(output));
 assert(IDirect3DDevice9_GetFVF(&device,output)==0x1234);
 {struct pw_d3d9_getter_request q={90,{0}};size_t bytes;memcpy(q.args,args,sizeof(q.args));assert(!pw_d3d9_getter_bytes(&q,&bytes));assert(!memcmp(output,payload,bytes));}
 assert(IDirect3DDevice9_GetFVF(&device,NULL)==D3DERR_INVALIDCALL);
 }
 {setup(95);
 float output[1024];memset(output,0xcc,sizeof(output));
 args[0]=2;
 args[1]=2;
 assert(IDirect3DDevice9_GetVertexShaderConstantF(&device,2,output,2)==0x1234);
 {struct pw_d3d9_getter_request q={95,{0}};size_t bytes;memcpy(q.args,args,sizeof(q.args));assert(!pw_d3d9_getter_bytes(&q,&bytes));assert(!memcmp(output,payload,bytes));}
 assert(IDirect3DDevice9_GetVertexShaderConstantF(&device,2,NULL,2)==D3DERR_INVALIDCALL);
 }
 {setup(97);
 int output[1024];memset(output,0xcc,sizeof(output));
 args[0]=2;
 args[1]=2;
 assert(IDirect3DDevice9_GetVertexShaderConstantI(&device,2,output,2)==0x1234);
 {struct pw_d3d9_getter_request q={97,{0}};size_t bytes;memcpy(q.args,args,sizeof(q.args));assert(!pw_d3d9_getter_bytes(&q,&bytes));assert(!memcmp(output,payload,bytes));}
 assert(IDirect3DDevice9_GetVertexShaderConstantI(&device,2,NULL,2)==D3DERR_INVALIDCALL);
 }
 {setup(99);
 BOOL output[1024];memset(output,0xcc,sizeof(output));
 args[0]=2;
 args[1]=2;
 assert(IDirect3DDevice9_GetVertexShaderConstantB(&device,2,output,2)==0x1234);
 {struct pw_d3d9_getter_request q={99,{0}};size_t bytes;memcpy(q.args,args,sizeof(q.args));assert(!pw_d3d9_getter_bytes(&q,&bytes));assert(!memcmp(output,payload,bytes));}
 assert(IDirect3DDevice9_GetVertexShaderConstantB(&device,2,NULL,2)==D3DERR_INVALIDCALL);
 }
 {setup(103);
 UINT output[1];memset(output,0xcc,sizeof(output));
 args[0]=2;
 assert(IDirect3DDevice9_GetStreamSourceFreq(&device,2,output)==0x1234);
 {struct pw_d3d9_getter_request q={103,{0}};size_t bytes;memcpy(q.args,args,sizeof(q.args));assert(!pw_d3d9_getter_bytes(&q,&bytes));assert(!memcmp(output,payload,bytes));}
 assert(IDirect3DDevice9_GetStreamSourceFreq(&device,2,NULL)==D3DERR_INVALIDCALL);
 }
 {setup(110);
 float output[1024];memset(output,0xcc,sizeof(output));
 args[0]=2;
 args[1]=2;
 assert(IDirect3DDevice9_GetPixelShaderConstantF(&device,2,output,2)==0x1234);
 {struct pw_d3d9_getter_request q={110,{0}};size_t bytes;memcpy(q.args,args,sizeof(q.args));assert(!pw_d3d9_getter_bytes(&q,&bytes));assert(!memcmp(output,payload,bytes));}
 assert(IDirect3DDevice9_GetPixelShaderConstantF(&device,2,NULL,2)==D3DERR_INVALIDCALL);
 }
 {setup(112);
 int output[1024];memset(output,0xcc,sizeof(output));
 args[0]=2;
 args[1]=2;
 assert(IDirect3DDevice9_GetPixelShaderConstantI(&device,2,output,2)==0x1234);
 {struct pw_d3d9_getter_request q={112,{0}};size_t bytes;memcpy(q.args,args,sizeof(q.args));assert(!pw_d3d9_getter_bytes(&q,&bytes));assert(!memcmp(output,payload,bytes));}
 assert(IDirect3DDevice9_GetPixelShaderConstantI(&device,2,NULL,2)==D3DERR_INVALIDCALL);
 }
 {setup(114);
 BOOL output[1024];memset(output,0xcc,sizeof(output));
 args[0]=2;
 args[1]=2;
 assert(IDirect3DDevice9_GetPixelShaderConstantB(&device,2,output,2)==0x1234);
 {struct pw_d3d9_getter_request q={114,{0}};size_t bytes;memcpy(q.args,args,sizeof(q.args));assert(!pw_d3d9_getter_bytes(&q,&bytes));assert(!memcmp(output,payload,bytes));}
 assert(IDirect3DDevice9_GetPixelShaderConstantB(&device,2,NULL,2)==D3DERR_INVALIDCALL);
 }
 {DWORD out=0xabcdef12;setup(58);args[0]=2;
 for(mode=1;mode<=4;mode++){HRESULT hr=IDirect3DDevice9_GetRenderState(&device,2,&out);assert(hr==(mode==1?D3DERR_INVALIDCALL:E_FAIL));assert(out==0xabcdef12);}
 mode=0;assert(failures==3);}
 {float out=123;setup(95);args[0]=2;args[1]=0;assert(IDirect3DDevice9_GetVertexShaderConstantF(&device,2,&out,0)==0x1234);assert(out==123);}
 {unsigned before=calls;assert(IDirect3DDevice9_Clear(&device,257,NULL,0,0,0,0)==D3DERR_INVALIDCALL);assert(calls==before);}
 setup(43);assert(IDirect3DDevice9_Clear(&device,0,NULL,0,0,0,0)==0x1234);
 /* Count normalization runs before reading any caller bytes. */
 {const unsigned methods[]={94,96,98,109,111,113};const DWORD modes[]={0x40,0x80,0x20};
 for(unsigned f=0;f<3;f++)for(unsigned m=0;m<6;m++){
  uint32_t values[1024];memset(values,0xa5,sizeof(values));creation_flags=modes[f];
  unsigned vertex=m<3,floating=m==0||m==3,stride=(m==2||m==5)?4:16;
  UINT software=vertex?(floating?8192u:2048u):(floating?224u:16u);
  UINT hardware=vertex&&f==0?(floating?256u:16u):software;
  setup(methods[m]);args[0]=software;args[1]=0;
  assert(invoke_constant(&device,methods[m],software,NULL,0)==0x1234);
  unsigned before=calls;
  assert(invoke_constant(&device,methods[m],software+1,NULL,0)==D3DERR_INVALIDCALL);
  assert(invoke_constant(&device,methods[m],UINT32_MAX,values,1)==D3DERR_INVALIDCALL);
  assert(invoke_constant(&device,methods[m],0,NULL,1)==D3DERR_INVALIDCALL);assert(calls==before);
  setup(methods[m]);args[0]=hardware;args[1]=0;
  /* A poisoned non-NULL address cannot be read when effective count is zero. */
  assert(invoke_constant(&device,methods[m],hardware,(void *)(uintptr_t)1,software-hardware)==0x1234);
  setup(methods[m]);args[0]=hardware-1;args[1]=1;expected_bytes=stride;
  uint32_t one[4]={0xa5a5a5a5,0xa5a5a5a5,0xa5a5a5a5,0xa5a5a5a5};
  if(stride==4){uint32_t truth=1;memcpy(payload,&truth,4);}
  assert(invoke_constant(&device,methods[m],hardware-1,one,software-hardware+1)==0x1234);
  setup(methods[m]);args[0]=0;args[1]=hardware<PW_D3D9_COMMAND_DATA/stride?hardware:PW_D3D9_COMMAND_DATA/stride;expected_bytes=args[1]*stride;
  if(stride==4)for(unsigned i=0;i<args[1];i++){uint32_t truth=1;memcpy(payload+4*i,&truth,4);}
  assert(invoke_constant(&device,methods[m],0,values,args[1])==0x1234);
  if(vertex&&f){before=calls;assert(invoke_constant(&device,methods[m],0,values,PW_D3D9_COMMAND_DATA/stride+1)==D3DERR_INVALIDCALL);assert(calls==before);}
 }
 creation_flags=D3DCREATE_HARDWARE_VERTEXPROCESSING;
 creation_result=E_FAIL;unsigned before=calls;assert(IDirect3DDevice9_SetVertexShaderConstantF(&device,0,NULL,0)==E_FAIL&&calls==before);creation_result=S_OK;
 }
 { float values[1024];setup(95);args[0]=2;args[1]=256;assert(IDirect3DDevice9_GetVertexShaderConstantF(&device,2,values,256)==0x1234);assert(!memcmp(values,payload,4096));args[1]=0;assert(IDirect3DDevice9_GetVertexShaderConstantF(&device,2,values,0)==0x1234);assert(IDirect3DDevice9_GetVertexShaderConstantF(&device,2,NULL,0)==D3DERR_INVALIDCALL);assert(IDirect3DDevice9_GetVertexShaderConstantF(&device,2,values,257)==D3DERR_INVALIDCALL);}
 { int values[1024];setup(97);args[0]=2;args[1]=256;assert(IDirect3DDevice9_GetVertexShaderConstantI(&device,2,values,256)==0x1234);assert(!memcmp(values,payload,4096));args[1]=0;assert(IDirect3DDevice9_GetVertexShaderConstantI(&device,2,values,0)==0x1234);assert(IDirect3DDevice9_GetVertexShaderConstantI(&device,2,NULL,0)==D3DERR_INVALIDCALL);assert(IDirect3DDevice9_GetVertexShaderConstantI(&device,2,values,257)==D3DERR_INVALIDCALL);}
 { BOOL values[1024];setup(99);args[0]=2;args[1]=1024;assert(IDirect3DDevice9_GetVertexShaderConstantB(&device,2,values,1024)==0x1234);assert(!memcmp(values,payload,4096));args[1]=0;assert(IDirect3DDevice9_GetVertexShaderConstantB(&device,2,values,0)==0x1234);assert(IDirect3DDevice9_GetVertexShaderConstantB(&device,2,NULL,0)==D3DERR_INVALIDCALL);assert(IDirect3DDevice9_GetVertexShaderConstantB(&device,2,values,1025)==D3DERR_INVALIDCALL);}
 { float values[1024];setup(110);args[0]=2;args[1]=256;assert(IDirect3DDevice9_GetPixelShaderConstantF(&device,2,values,256)==0x1234);assert(!memcmp(values,payload,4096));args[1]=0;assert(IDirect3DDevice9_GetPixelShaderConstantF(&device,2,values,0)==0x1234);assert(IDirect3DDevice9_GetPixelShaderConstantF(&device,2,NULL,0)==D3DERR_INVALIDCALL);assert(IDirect3DDevice9_GetPixelShaderConstantF(&device,2,values,257)==D3DERR_INVALIDCALL);}
 { int values[1024];setup(112);args[0]=2;args[1]=256;assert(IDirect3DDevice9_GetPixelShaderConstantI(&device,2,values,256)==0x1234);assert(!memcmp(values,payload,4096));args[1]=0;assert(IDirect3DDevice9_GetPixelShaderConstantI(&device,2,values,0)==0x1234);assert(IDirect3DDevice9_GetPixelShaderConstantI(&device,2,NULL,0)==D3DERR_INVALIDCALL);assert(IDirect3DDevice9_GetPixelShaderConstantI(&device,2,values,257)==D3DERR_INVALIDCALL);}
 { BOOL values[1024];setup(114);args[0]=2;args[1]=1024;assert(IDirect3DDevice9_GetPixelShaderConstantB(&device,2,values,1024)==0x1234);assert(!memcmp(values,payload,4096));args[1]=0;assert(IDirect3DDevice9_GetPixelShaderConstantB(&device,2,values,0)==0x1234);assert(IDirect3DDevice9_GetPixelShaderConstantB(&device,2,NULL,0)==D3DERR_INVALIDCALL);assert(IDirect3DDevice9_GetPixelShaderConstantB(&device,2,values,1025)==D3DERR_INVALIDCALL);}
 {DWORD value;setup(58);args[0]=2;mode=5;assert(IDirect3DDevice9_GetRenderState(&device,2,&value)==0x1234 && device_refs==0);}
 printf("PASS typed device methods:40 command+28 getter slots, calls=%u resolve=%u failure=%u\n",calls,resolves,failures);return 0;
}
