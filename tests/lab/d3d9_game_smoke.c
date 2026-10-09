/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <d3dx9.h>
#include <stdio.h>
#include <string.h>
#include "d3d9_game_smoke_fx.h"
#define CHECK(x) do { HRESULT check_hr=(x); if(FAILED(check_hr)){fprintf(stderr,"PW_GAME_SMOKE line=%u hr=%08lx\n",__LINE__,(DWORD)check_hr);return 20;} } while(0)
#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"PW_GAME_SMOKE line=%u assertion\n",__LINE__);return 21;} } while(0)
static LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcW(w,m,a,b);}
struct vertex {float x,y,z,w;DWORD color;};
static const char effect_source[]=
 "float4 tint; float4 pixel() : COLOR { return tint; }"
 "technique T { pass P { ZEnable=false; CullMode=1; AlphaBlendEnable=false; "
 "PixelShader=compile ps_2_0 pixel(); } }\n";
int wmain(int argc,WCHAR **argv)
{
 REQUIRE(argc>=6&&argc<=8);int fullscreen=0,source_effect=0;
 for(int i=6;i<argc;i++){if(!wcscmp(argv[i],L"--fullscreen"))fullscreen=1;else if(!wcscmp(argv[i],L"--source-effect"))source_effect=1;else REQUIRE(0);}
 REQUIRE(SetEnvironmentVariableW(L"PW_D3D9_SERVICE64",argv[2]));
 REQUIRE(SetEnvironmentVariableW(L"PW_D3D9_BACKEND64",argv[3]));
 HMODULE dll=LoadLibraryW(argv[1]),d3dx=LoadLibraryW(argv[4]);REQUIRE(dll&&d3dx);
 IDirect3D9 *(WINAPI *create)(UINT)=(void *)GetProcAddress(dll,"Direct3DCreate9");
 HRESULT (WINAPI *create_effect)(IDirect3DDevice9 *,const void *,UINT,const D3DXMACRO *,ID3DXInclude *,DWORD,ID3DXEffectPool *,ID3DXEffect **,ID3DXBuffer **)=(void *)GetProcAddress(d3dx,"D3DXCreateEffect");
 HRESULT (WINAPI *create_sphere)(IDirect3DDevice9 *,FLOAT,UINT,UINT,ID3DXMesh **,ID3DXBuffer **)=(void *)GetProcAddress(d3dx,"D3DXCreateSphere");
 HRESULT (WINAPI *create_texture)(IDirect3DDevice9 *,const char *,IDirect3DTexture9 **)=(void *)GetProcAddress(d3dx,"D3DXCreateTextureFromFileA");
 REQUIRE(create&&create_effect&&create_sphere&&create_texture);
 char bitmap[MAX_PATH];REQUIRE(WideCharToMultiByte(CP_ACP,0,argv[5],-1,bitmap,sizeof(bitmap),NULL,NULL));
 WNDCLASSW cls={.lpfnWndProc=proc,.hInstance=GetModuleHandleW(NULL),.lpszClassName=L"PW_GAME_SMOKE"};REQUIRE(RegisterClassW(&cls));
 HWND window=CreateWindowW(cls.lpszClassName,L"Production bridge smoke",WS_OVERLAPPEDWINDOW|WS_VISIBLE,0,0,640,480,NULL,NULL,cls.hInstance,NULL);REQUIRE(window);
 for(unsigned cycle=0;cycle<3;cycle++){
  IDirect3D9 *factory=create(D3D_SDK_VERSION);REQUIRE(factory);
  D3DPRESENT_PARAMETERS pp={.BackBufferWidth=fullscreen?1920:640,.BackBufferHeight=fullscreen?1080:480,.BackBufferFormat=D3DFMT_A8R8G8B8,.BackBufferCount=1,.SwapEffect=fullscreen?D3DSWAPEFFECT_FLIP:D3DSWAPEFFECT_DISCARD,.hDeviceWindow=window,.Windowed=!fullscreen,.EnableAutoDepthStencil=TRUE,.AutoDepthStencilFormat=D3DFMT_D16,.PresentationInterval=D3DPRESENT_INTERVAL_DEFAULT};
  IDirect3DDevice9 *device=NULL;CHECK(IDirect3D9_CreateDevice(factory,0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&device));REQUIRE(device);
  fprintf(stderr,"PW_GAME_SMOKE stage=reset cycle=%u\n",cycle);
  CHECK(IDirect3DDevice9_Reset(device,&pp));
  fprintf(stderr,"PW_GAME_SMOKE stage=targets cycle=%u\n",cycle);
  IDirect3DSurface9 *original=NULL,*depth=NULL,*target=NULL,*readback=NULL;
  CHECK(IDirect3DDevice9_GetRenderTarget(device,0,&original));CHECK(IDirect3DDevice9_GetDepthStencilSurface(device,&depth));
  CHECK(IDirect3DDevice9_CreateRenderTarget(device,64,64,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&target,NULL));
  CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(device,64,64,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,NULL));
  CHECK(IDirect3DDevice9_SetRenderTarget(device,0,target));CHECK(IDirect3DDevice9_SetDepthStencilSurface(device,NULL));
  fprintf(stderr,"PW_GAME_SMOKE stage=texture cycle=%u\n",cycle);
  IDirect3DTexture9 *texture=NULL,*bound_texture=NULL;CHECK(create_texture(device,bitmap,&texture));REQUIRE(texture);
  CHECK(IDirect3DDevice9_SetTexture(device,0,(IDirect3DBaseTexture9 *)texture));CHECK(IDirect3DDevice9_GetTexture(device,0,(IDirect3DBaseTexture9 **)&bound_texture));REQUIRE(bound_texture==texture);IDirect3DTexture9_Release(bound_texture);
  fprintf(stderr,"PW_GAME_SMOKE stage=mesh cycle=%u\n",cycle);
  ID3DXMesh *sphere=NULL;CHECK(create_sphere(device,1.0f,8,8,&sphere,NULL));REQUIRE(sphere&&sphere->lpVtbl->GetNumFaces(sphere)>0);sphere->lpVtbl->Release(sphere);
  fprintf(stderr,"PW_GAME_SMOKE stage=buffers cycle=%u\n",cycle);
  IDirect3DVertexBuffer9 *vb=NULL;IDirect3DIndexBuffer9 *ib=NULL;
  const struct vertex vertices[3]={{0,0,0,1,0xffffffff},{64,0,0,1,0xffffffff},{32,64,0,1,0xffffffff}};const WORD indices[3]={0,1,2};void *memory=NULL;
  CHECK(IDirect3DDevice9_CreateVertexBuffer(device,sizeof(vertices),0,D3DFVF_XYZRHW|D3DFVF_DIFFUSE,D3DPOOL_MANAGED,&vb,NULL));
  CHECK(IDirect3DVertexBuffer9_Lock(vb,0,sizeof(vertices),&memory,0));memcpy(memory,vertices,sizeof(vertices));CHECK(IDirect3DVertexBuffer9_Unlock(vb));
  CHECK(IDirect3DDevice9_CreateIndexBuffer(device,sizeof(indices),0,D3DFMT_INDEX16,D3DPOOL_MANAGED,&ib,NULL));CHECK(IDirect3DIndexBuffer9_Lock(ib,0,sizeof(indices),&memory,0));memcpy(memory,indices,sizeof(indices));CHECK(IDirect3DIndexBuffer9_Unlock(ib));
  CHECK(IDirect3DDevice9_SetStreamSource(device,0,vb,0,sizeof(struct vertex)));CHECK(IDirect3DDevice9_SetIndices(device,ib));CHECK(IDirect3DDevice9_SetFVF(device,D3DFVF_XYZRHW|D3DFVF_DIFFUSE));
  fprintf(stderr,"PW_GAME_SMOKE stage=effect cycle=%u\n",cycle);
  CHECK(IDirect3DDevice9_SetVertexShader(device,NULL));
  ID3DXEffect *effect=NULL;ID3DXBuffer *errors=NULL;HRESULT hr=create_effect(device,source_effect?(const void *)effect_source:(const void *)test_effect_states_effect_blob,source_effect?sizeof(effect_source)-1:sizeof(test_effect_states_effect_blob),NULL,NULL,0,NULL,&effect,&errors);
  if(errors){fprintf(stderr,"D3DX: %s\n",(char *)ID3DXBuffer_GetBufferPointer(errors));ID3DXBuffer_Release(errors);}CHECK(hr);REQUIRE(effect);
  fprintf(stderr,"PW_GAME_SMOKE stage=constants cycle=%u\n",cycle);
  const FLOAT red[4]={1,0,0,1};IDirect3DPixelShader9 *pixel_shader=NULL;
  if(source_effect){D3DXHANDLE tint=effect->lpVtbl->GetParameterByName(effect,NULL,"tint");REQUIRE(tint);D3DXVECTOR4 color={1,0,0,1};CHECK(effect->lpVtbl->SetVector(effect,tint,&color));}
  else {
   D3DXHANDLE camera=effect->lpVtbl->GetParameterByName(effect,NULL,"camera");REQUIRE(camera);
   D3DXMATRIX matrix={.m={{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};CHECK(effect->lpVtbl->SetMatrix(effect,camera,&matrix));
   const DWORD pixel[]={0xffff0200,0x02000001,0x800f0800,0xa0e40000,0xffff};CHECK(IDirect3DDevice9_CreatePixelShader(device,pixel,&pixel_shader));
   CHECK(IDirect3DDevice9_SetPixelShader(device,pixel_shader));CHECK(IDirect3DDevice9_SetPixelShaderConstantF(device,0,red,1));
  }
  CHECK(IDirect3DDevice9_SetRenderState(device,D3DRS_CULLMODE,D3DCULL_NONE));
  CHECK(IDirect3DDevice9_SetRenderState(device,D3DRS_ZENABLE,D3DZB_FALSE));
  CHECK(IDirect3DDevice9_Clear(device,0,NULL,D3DCLEAR_TARGET,0xff000000,1,0));CHECK(IDirect3DDevice9_BeginScene(device));
  fprintf(stderr,"PW_GAME_SMOKE stage=effect-begin cycle=%u\n",cycle);
  UINT passes=0;CHECK(effect->lpVtbl->Begin(effect,&passes,0));REQUIRE(passes==1);CHECK(effect->lpVtbl->BeginPass(effect,0));CHECK(effect->lpVtbl->CommitChanges(effect));
  if(!source_effect){
   DWORD active=0;CHECK(IDirect3DDevice9_GetRenderState(device,D3DRS_ZENABLE,&active));REQUIRE(active==D3DZB_TRUE);
   CHECK(IDirect3DDevice9_SetVertexShader(device,NULL));CHECK(IDirect3DDevice9_SetRenderState(device,D3DRS_ZENABLE,D3DZB_FALSE));
  }
  CHECK(IDirect3DDevice9_DrawIndexedPrimitive(device,D3DPT_TRIANGLELIST,0,0,3,0,1));CHECK(effect->lpVtbl->EndPass(effect));CHECK(effect->lpVtbl->End(effect));CHECK(IDirect3DDevice9_EndScene(device));
  fprintf(stderr,"PW_GAME_SMOKE stage=readback cycle=%u\n",cycle);
  DWORD restored=0;CHECK(IDirect3DDevice9_GetRenderState(device,D3DRS_ZENABLE,&restored));REQUIRE(restored==D3DZB_FALSE);
  CHECK(IDirect3DDevice9_GetRenderTargetData(device,target,readback));D3DLOCKED_RECT lock;CHECK(IDirect3DSurface9_LockRect(readback,&lock,NULL,D3DLOCK_READONLY));
  DWORD center;memcpy(&center,(char *)lock.pBits+32*lock.Pitch+32*4,4);CHECK(IDirect3DSurface9_UnlockRect(readback));REQUIRE((center&0xffffff)==0xff0000);
  CHECK(IDirect3DDevice9_SetRenderTarget(device,0,original));CHECK(IDirect3DDevice9_SetDepthStencilSurface(device,depth));
  CHECK(IDirect3DDevice9_SetTexture(device,0,NULL));CHECK(IDirect3DDevice9_SetStreamSource(device,0,NULL,0,0));CHECK(IDirect3DDevice9_SetIndices(device,NULL));
  CHECK(IDirect3DDevice9_SetPixelShader(device,NULL));if(pixel_shader)IDirect3DPixelShader9_Release(pixel_shader);
  effect->lpVtbl->Release(effect);IDirect3DTexture9_Release(texture);IDirect3DVertexBuffer9_Release(vb);IDirect3DIndexBuffer9_Release(ib);
  IDirect3DSurface9_Release(readback);IDirect3DSurface9_Release(target);IDirect3DSurface9_Release(depth);IDirect3DSurface9_Release(original);
  CHECK(IDirect3DDevice9_Present(device,NULL,NULL,NULL,NULL));REQUIRE(IDirect3DDevice9_Release(device)==0);REQUIRE(IDirect3D9_Release(factory)==0);
  printf("PW_GAME_SMOKE cycle=%u effect=1 mesh=1 texture=1 restored=1 pixel=%08lx present=1 status=0\n",cycle,center);fflush(stdout);
 }
 DestroyWindow(window);UnregisterClassW(cls.lpszClassName,cls.hInstance);FreeLibrary(d3dx);FreeLibrary(dll);return 0;
}
