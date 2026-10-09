/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "../../wine/ps5/d3d9/pw_d3d9_api_observe.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
static unsigned calls,logs;
static unsigned output_logs,memory_logs,unreadable_logs,caps_logs,rect_logs;
static int enabled,reenter;
static HRESULT query_result=E_NOINTERFACE;
#ifdef PW_D3D9_API_OBSERVE_TEST
#define EXPECT_LOGS(n) (enabled?(n):0u)
#else
#define EXPECT_LOGS(n) 0u
#endif
static IDirect3DDevice9 device;
static const GUID guid={0x12345678,0x9abc,0xdef0,{0x12,0x34,0x56,0x78,0x9a,0xbc,0xde,0xf0}};
void pw_d3d9_api_test_output(const char *text)
{
 if(strstr(text,"PW_D3D9_API_OUTPUT ")){
  assert(strstr(text," self.address="));
  ++output_logs;
  if(strstr(text,"method=GetAvailableTextureMem")){++memory_logs;assert(strstr(text,"result=fedcba98"));}
  if(strstr(text,"unreadable"))++unreadable_logs;
  if(strstr(text,"method=GetDeviceCaps")&&strstr(text,"a0000000")){++caps_logs;assert(strstr(text,"a000004b"));}
  if(strstr(text,"method=LockRect")&&strstr(text,"rect=00000001,00000002,00000003,00000004")){++rect_logs;assert(strstr(text,"Pitch=-64,pBits="));}
  errno=EDOM;SetLastError(0x1111);return;
 }
 assert(strstr(text,"PW_D3D9_API_FAIL interface=IDirect3DDevice9 slot=") || strstr(text,"PW_D3D9_API_RESULT interface=IDirect3DDevice9 slot="));
 assert(strstr(text," caller=") && !strstr(text,"caller=00000000 "));
 if(strstr(text,"method=QueryInterface"))assert(strstr(text,"iid=12345678-9abc-def0-1234-56789abcdef0")||strstr(text,"iid=unreadable")||strstr(text,"iid=none"));
 ++logs;errno=EDOM;SetLastError(0x1111);
}
static HRESULT WINAPI query(IDirect3DDevice9 *self,REFIID iid,void **out)
{assert(self==&device);(void)iid;++calls;if(out)*out=NULL;errno=ERANGE;SetLastError(0xbeef);return query_result;}
static HRESULT WINAPI validate(IDirect3DDevice9 *self,DWORD *passes)
{
 assert(self==&device);++calls;
 if(reenter){reenter=0;void *out=(void *)1;assert(IDirect3DDevice9_QueryInterface(self,&guid,&out)==E_NOINTERFACE&&!out);}
 if(passes)*passes=7;
 errno=ERANGE;SetLastError(0xbeef);return D3DERR_INVALIDCALL;
}
static HRESULT WINAPI draw(IDirect3DDevice9 *self,D3DPRIMITIVETYPE kind,UINT min,UINT vertices,UINT primitives,const void *indices,D3DFORMAT format,const void *data,UINT stride)
{assert(self==&device&&kind==D3DPT_TRIANGLELIST&&min==5&&vertices==11&&primitives==13&&indices==(void *)17&&format==D3DFMT_INDEX32&&data==(void *)19&&stride==23);++calls;return S_FALSE;}
static void WINAPI position(IDirect3DDevice9 *self,int x,int y,DWORD flags)
{assert(self==&device&&x==-123&&y==456&&flags==0x87654321);++calls;}
static BOOL WINAPI show(IDirect3DDevice9 *self,BOOL value)
{assert(self==&device&&value==(BOOL)0x81234567);++calls;return (BOOL)0xfedcba98;}
static float WINAPI patches(IDirect3DDevice9 *self){assert(self==&device);++calls;return 3.25f;}
static HRESULT WINAPI destroyed(IDirect3DDevice9 *self)
{++calls;if(self==&device)return S_OK;HeapFree(GetProcessHeap(),0,self);return E_FAIL;}
static void mark(void){errno=ERANGE;SetLastError(0xbeef);}
static UINT WINAPI memory(IDirect3DDevice9 *self){(void)self;mark();return 0xfedcba98u;}
static HRESULT WINAPI caps(IDirect3DDevice9 *self,D3DCAPS9 *out)
{
 if((uintptr_t)out>1){DWORD words[sizeof(*out)/4];for(unsigned i=0;i<sizeof(words)/4;i++)words[i]=0xa0000000u+i;memcpy(out,words,sizeof(words));}
 if(self!=&device)HeapFree(GetProcessHeap(),0,self);
 mark();return S_OK;
}
static HRESULT WINAPI create_texture(IDirect3DDevice9 *self,UINT w,UINT h,UINT levels,DWORD usage,D3DFORMAT format,D3DPOOL pool,IDirect3DTexture9 **out,HANDLE *shared)
{assert(self==&device&&w==64&&h==32&&levels==1&&usage==D3DUSAGE_DEPTHSTENCIL&&format==(D3DFORMAT)MAKEFOURCC('I','N','T','Z')&&pool==D3DPOOL_DEFAULT&&!shared);*out=(void *)(uintptr_t)0x12345678;mark();return S_OK;}
static HRESULT WINAPI create_rt(IDirect3DDevice9 *self,UINT w,UINT h,D3DFORMAT f,D3DMULTISAMPLE_TYPE sample,DWORD quality,BOOL lockable,IDirect3DSurface9 **out,HANDLE *shared)
{assert(self==&device&&w==64&&h==32&&f==D3DFMT_A8R8G8B8&&sample==D3DMULTISAMPLE_NONE&&quality==0&&lockable==TRUE&&!shared);*out=(void *)(uintptr_t)0x23456789;mark();return S_OK;}
static HRESULT WINAPI create_depth(IDirect3DDevice9 *self,UINT w,UINT h,D3DFORMAT f,D3DMULTISAMPLE_TYPE sample,DWORD quality,BOOL discard,IDirect3DSurface9 **out,HANDLE *shared)
{assert(self==&device&&w==64&&h==32&&f==D3DFMT_D16&&sample==D3DMULTISAMPLE_NONE&&!quality&&discard==TRUE&&!shared);*out=(void *)(uintptr_t)0x3456789a;mark();return S_OK;}
static HRESULT WINAPI create_offscreen(IDirect3DDevice9 *self,UINT w,UINT h,D3DFORMAT f,D3DPOOL pool,IDirect3DSurface9 **out,HANDLE *shared)
{assert(self==&device&&w==64&&h==32&&f==D3DFMT_A8R8G8B8&&pool==D3DPOOL_SYSTEMMEM&&!shared);*out=(void *)(uintptr_t)0x456789ab;mark();return S_OK;}
static HRESULT WINAPI get_rt(IDirect3DDevice9 *self,DWORD index,IDirect3DSurface9 **out)
{assert(self==&device&&index==2);*out=(void *)(uintptr_t)0x56789abc;mark();return S_OK;}
static HRESULT WINAPI get_depth(IDirect3DDevice9 *self,IDirect3DSurface9 **out)
{assert(self==&device);*out=(void *)(uintptr_t)0x6789abcd;mark();return S_OK;}
static HRESULT WINAPI get_back(IDirect3DDevice9 *self,UINT chain,UINT index,D3DBACKBUFFER_TYPE type,IDirect3DSurface9 **out)
{assert(self==&device&&chain==3&&index==4&&type==D3DBACKBUFFER_TYPE_MONO);*out=(void *)(uintptr_t)0x789abcde;mark();return S_OK;}
static HRESULT WINAPI surface_desc(IDirect3DSurface9 *self,D3DSURFACE_DESC *out)
{(void)self;*out=(D3DSURFACE_DESC){D3DFMT_D16,D3DRTYPE_SURFACE,D3DUSAGE_DEPTHSTENCIL,D3DPOOL_DEFAULT,D3DMULTISAMPLE_NONE,0,64,32};mark();return S_OK;}
static HRESULT WINAPI surface_lock(IDirect3DSurface9 *self,D3DLOCKED_RECT *out,const RECT *rect,DWORD flags)
{(void)self;assert(flags==D3DLOCK_READONLY);if((uintptr_t)out>1)*out=(D3DLOCKED_RECT){-64,(void *)(uintptr_t)0x13572468};if((uintptr_t)rect>1)((RECT *)rect)->left=99;mark();return S_OK;}
static HRESULT WINAPI texture_desc(IDirect3DTexture9 *self,UINT level,D3DSURFACE_DESC *out)
{(void)self;assert(level==2);return surface_desc(NULL,out);}
static DWORD WINAPI levels(IDirect3DTexture9 *self){(void)self;mark();return 7;}
static D3DRESOURCETYPE WINAPI type(IDirect3DTexture9 *self){(void)self;mark();return D3DRTYPE_TEXTURE;}
static UINT WINAPI modes(IDirect3D9 *self,UINT adapter,D3DFORMAT format)
{(void)self;assert(adapter==3&&format==D3DFMT_A8R8G8B8);mark();return 12;}
static HRESULT WINAPI factory_caps(IDirect3D9 *self,UINT adapter,D3DDEVTYPE type,D3DCAPS9 *out)
{(void)self;assert(adapter==2&&type==D3DDEVTYPE_HAL);return caps(&device,out);}
static void check_outputs(void)
{
 D3DCAPS9 c;IDirect3DTexture9 *t;IDirect3DSurface9 *s;D3DSURFACE_DESC d;
 for(unsigned i=0;i<80;i++)assert(IDirect3DDevice9_GetAvailableTextureMem(&device)==0xfedcba98u&&errno==ERANGE&&GetLastError()==0xbeef);
 assert(IDirect3DDevice9_GetDeviceCaps(&device,&c)==S_OK&&errno==ERANGE&&GetLastError()==0xbeef);
 assert(IDirect3DDevice9_GetDeviceCaps(&device,(void *)(uintptr_t)1)==S_OK);assert(IDirect3DDevice9_GetDeviceCaps(&device,NULL)==S_OK);
 IDirect3DDevice9 *dying=HeapAlloc(GetProcessHeap(),0,sizeof(*dying));assert(dying);dying->lpVtbl=device.lpVtbl;assert(IDirect3DDevice9_GetDeviceCaps(dying,&c)==S_OK);
 assert(IDirect3DDevice9_CreateTexture(&device,64,32,1,D3DUSAGE_DEPTHSTENCIL,(D3DFORMAT)MAKEFOURCC('I','N','T','Z'),D3DPOOL_DEFAULT,&t,NULL)==S_OK&&t==(void *)(uintptr_t)0x12345678);
 assert(IDirect3DDevice9_CreateRenderTarget(&device,64,32,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,TRUE,&s,NULL)==S_OK);
 assert(IDirect3DDevice9_CreateDepthStencilSurface(&device,64,32,D3DFMT_D16,D3DMULTISAMPLE_NONE,0,TRUE,&s,NULL)==S_OK);
 assert(IDirect3DDevice9_CreateOffscreenPlainSurface(&device,64,32,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&s,NULL)==S_OK);
 assert(IDirect3DDevice9_GetRenderTarget(&device,2,&s)==S_OK);assert(IDirect3DDevice9_GetDepthStencilSurface(&device,&s)==S_OK);assert(IDirect3DDevice9_GetBackBuffer(&device,3,4,D3DBACKBUFFER_TYPE_MONO,&s)==S_OK);
 static const IDirect3DSurface9Vtbl surface_raw={.GetDesc=surface_desc,.LockRect=surface_lock};
 IDirect3DSurface9 surface={(IDirect3DSurface9Vtbl *)pw_d3d9_api_observe_IDirect3DSurface9(&surface_raw)};
 RECT rect={1,2,3,4};D3DLOCKED_RECT locked;assert(IDirect3DSurface9_GetDesc(&surface,&d)==S_OK);
 assert(IDirect3DSurface9_LockRect(&surface,&locked,&rect,D3DLOCK_READONLY)==S_OK&&rect.left==99&&locked.Pitch==-64);
 assert(IDirect3DSurface9_LockRect(&surface,(void *)(uintptr_t)1,(void *)(uintptr_t)1,D3DLOCK_READONLY)==S_OK&&errno==ERANGE&&GetLastError()==0xbeef);
 static const IDirect3DTexture9Vtbl texture_raw={.GetLevelDesc=texture_desc,.GetLevelCount=levels,.GetType=type};
 IDirect3DTexture9 texture={(IDirect3DTexture9Vtbl *)pw_d3d9_api_observe_IDirect3DTexture9(&texture_raw)};
 assert(IDirect3DTexture9_GetLevelDesc(&texture,2,&d)==S_OK);assert(IDirect3DTexture9_GetLevelCount(&texture)==7);assert(IDirect3DTexture9_GetType(&texture)==D3DRTYPE_TEXTURE);
 static const IDirect3D9Vtbl factory_raw={.GetAdapterModeCount=modes,.GetDeviceCaps=factory_caps};
 IDirect3D9 factory={(IDirect3D9Vtbl *)pw_d3d9_api_observe_IDirect3D9(&factory_raw)};
 assert(IDirect3D9_GetAdapterModeCount(&factory,3,D3DFMT_A8R8G8B8)==12);assert(IDirect3D9_GetDeviceCaps(&factory,2,D3DDEVTYPE_HAL,&c)==S_OK);
 assert(output_logs==EXPECT_LOGS(83u)&&memory_logs==EXPECT_LOGS(64u)&&caps_logs==EXPECT_LOGS(3u)&&rect_logs==EXPECT_LOGS(1u));
 assert(unreadable_logs==EXPECT_LOGS(7u));
}
int main(int argc,char **argv)
{
 (void)argv;enabled=argc>1;SetEnvironmentVariableA("PW_D3D9_DIAGNOSTICS",enabled?"1":"0");
 static const IDirect3DDevice9Vtbl raw={.QueryInterface=query,.ValidateDevice=validate,.DrawIndexedPrimitiveUP=draw,.SetCursorPosition=position,.ShowCursor=show,.GetNPatchMode=patches,.TestCooperativeLevel=destroyed,.GetAvailableTextureMem=memory,.GetDeviceCaps=caps,.CreateTexture=create_texture,.CreateRenderTarget=create_rt,.CreateDepthStencilSurface=create_depth,.CreateOffscreenPlainSurface=create_offscreen,.GetRenderTarget=get_rt,.GetDepthStencilSurface=get_depth,.GetBackBuffer=get_back};
 const IDirect3DDevice9Vtbl *wrapped=pw_d3d9_api_observe_IDirect3DDevice9(&raw);assert(wrapped&&((wrapped!=&raw)==enabled));device.lpVtbl=(IDirect3DDevice9Vtbl *)wrapped;
 assert(pw_d3d9_api_original_vtable(wrapped)==&raw);assert(pw_d3d9_api_original_vtable(NULL)==NULL);
 IDirect3DDevice9Vtbl foreign=raw;assert(pw_d3d9_api_observe_IDirect3DDevice9(&foreign)==(enabled?NULL:&foreign));
 DWORD passes=0;reenter=1;assert(IDirect3DDevice9_ValidateDevice(&device,&passes)==D3DERR_INVALIDCALL&&passes==7&&GetLastError()==0xbeef&&errno==ERANGE);assert(calls==2&&logs==EXPECT_LOGS(2u));
 assert(IDirect3DDevice9_ValidateDevice(&device,NULL)==D3DERR_INVALIDCALL&&GetLastError()==0xbeef&&errno==ERANGE);
 void *out=(void *)1;assert(IDirect3DDevice9_QueryInterface(&device,(REFIID)(uintptr_t)1,&out)==E_NOINTERFACE&&!out&&GetLastError()==0xbeef&&errno==ERANGE);
 assert(IDirect3DDevice9_QueryInterface(&device,NULL,NULL)==E_NOINTERFACE);
 assert(IDirect3DDevice9_DrawIndexedPrimitiveUP(&device,D3DPT_TRIANGLELIST,5,11,13,(void *)17,D3DFMT_INDEX32,(void *)19,23)==S_FALSE);
 IDirect3DDevice9_SetCursorPosition(&device,-123,456,0x87654321);assert(IDirect3DDevice9_ShowCursor(&device,(BOOL)0x81234567)==(BOOL)0xfedcba98);assert(IDirect3DDevice9_GetNPatchMode(&device)==3.25f);
 query_result=S_FALSE;assert(IDirect3DDevice9_QueryInterface(&device,&guid,&out)==S_FALSE&&!out&&GetLastError()==0xbeef&&errno==ERANGE);
 assert(IDirect3DDevice9_TestCooperativeLevel(&device)==S_OK);
 IDirect3DDevice9 *dying=HeapAlloc(GetProcessHeap(),0,sizeof(*dying));assert(dying);dying->lpVtbl=(IDirect3DDevice9Vtbl *)wrapped;assert(IDirect3DDevice9_TestCooperativeLevel(dying)==E_FAIL);
 assert(calls==12&&logs==EXPECT_LOGS(8u));check_outputs();printf("PW_API_OBSERVE enabled=%d calls=%u logs=%u abi=1 reentry=1 invalid_iid=1 destroyed_self=1 outputs=%u bounded=1 snapshot=1 status=0\n",enabled,calls,logs,output_logs);return 0;
}
