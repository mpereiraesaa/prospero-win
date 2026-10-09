/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do{HRESULT h=(x);if(h!=S_OK){fprintf(stderr,"TRANSFORM_FAIL line=%u hr=%08lx\n",__LINE__,(DWORD)h);return 20;}}while(0)
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"TRANSFORM_FAIL line=%u\n",__LINE__);return 21;}}while(0)
static D3DMATRIX matrix(unsigned seed)
{
 D3DMATRIX m={0};for(unsigned r=0;r<4;r++)for(unsigned c=0;c<4;c++)m.m[r][c]=(float)(seed+r*4+c)/16.0f;return m;
}
static void observe(IDirect3DDevice9 *d,unsigned cycle,const char *stage,unsigned state)
{
 D3DMATRIX m;memset(&m,0xa5,sizeof(m));HRESULT hr=IDirect3DDevice9_GetTransform(d,(D3DTRANSFORMSTATETYPE)state,&m);
 printf("TRANSFORM_VALUE cycle=%u stage=%s state=%u hr=%08lx bits=",cycle,stage,state,(DWORD)hr);
 for(unsigned n=0;n<16;n++){uint32_t word;memcpy(&word,(char *)&m+4*n,4);printf("%08lx",(DWORD)word);}puts("");fflush(stdout);
}
static void outcome(unsigned cycle,const char *stage,HRESULT hr)
{printf("TRANSFORM_RESULT cycle=%u stage=%s hr=%08lx\n",cycle,stage,(DWORD)hr);fflush(stdout);}
static int exercise(IDirect3DDevice9 *d,D3DPRESENT_PARAMETERS *pp,unsigned cycle)
{
 const unsigned selectors[]={2,3,16,23,256,511};D3DMATRIX a=matrix(1),b=matrix(17),c=matrix(33);
 for(unsigned n=0;n<sizeof(selectors)/sizeof(*selectors);n++){
  unsigned state=selectors[n];observe(d,cycle,"initial",state);CHECK(IDirect3DDevice9_SetTransform(d,(D3DTRANSFORMSTATETYPE)state,&a));
  for(unsigned repeat=0;repeat<4;repeat++)observe(d,cycle,"set-repeat",state);
 }
 /* Raw Set/Get copies must preserve negative zero and NaN payloads. */
 D3DMATRIX raw=a;const uint32_t special[]={0x80000000u,0x7fc01234u,0x7f800000u,0xff800000u};memcpy(&raw,special,sizeof(special));
 CHECK(IDirect3DDevice9_SetTransform(d,D3DTS_WORLD,&raw));observe(d,cycle,"raw-bits",D3DTS_WORLD);
 CHECK(IDirect3DDevice9_SetTransform(d,D3DTS_WORLD,&a));CHECK(IDirect3DDevice9_MultiplyTransform(d,D3DTS_WORLD,&b));
 observe(d,cycle,"multiply",D3DTS_WORLD);observe(d,cycle,"multiply-repeat",D3DTS_WORLD);
 IDirect3DStateBlock9 *block=NULL;
 CHECK(IDirect3DDevice9_SetTransform(d,D3DTS_WORLD,&a));CHECK(IDirect3DDevice9_BeginStateBlock(d));
 CHECK(IDirect3DDevice9_SetTransform(d,D3DTS_WORLD,&b));observe(d,cycle,"record-live",D3DTS_WORLD);
 CHECK(IDirect3DDevice9_MultiplyTransform(d,D3DTS_VIEW,&c));observe(d,cycle,"record-multiply",D3DTS_VIEW);
 CHECK(IDirect3DDevice9_EndStateBlock(d,&block));REQUIRE(block);observe(d,cycle,"end-live",D3DTS_WORLD);
 CHECK(IDirect3DStateBlock9_Apply(block));observe(d,cycle,"record-apply",D3DTS_WORLD);
 CHECK(IDirect3DDevice9_SetTransform(d,D3DTS_WORLD,&c));CHECK(IDirect3DStateBlock9_Capture(block));
 CHECK(IDirect3DDevice9_SetTransform(d,D3DTS_WORLD,&a));CHECK(IDirect3DStateBlock9_Apply(block));observe(d,cycle,"capture-apply",D3DTS_WORLD);
 CHECK(IDirect3DDevice9_BeginStateBlock(d));outcome(cycle,"capture-during-record",IDirect3DStateBlock9_Capture(block));outcome(cycle,"apply-during-record",IDirect3DStateBlock9_Apply(block));
 IDirect3DStateBlock9 *empty=NULL;CHECK(IDirect3DDevice9_EndStateBlock(d,&empty));REQUIRE(empty);IDirect3DStateBlock9_Release(empty);IDirect3DStateBlock9_Release(block);
 for(unsigned type=1;type<=3;type++){
  CHECK(IDirect3DDevice9_SetTransform(d,D3DTS_WORLD,&a));CHECK(IDirect3DDevice9_CreateStateBlock(d,(D3DSTATEBLOCKTYPE)type,&block));
  CHECK(IDirect3DDevice9_SetTransform(d,D3DTS_WORLD,&b));CHECK(IDirect3DStateBlock9_Apply(block));
  observe(d,cycle,type==1?"all-apply":type==2?"pixel-apply":"vertex-apply",D3DTS_WORLD);IDirect3DStateBlock9_Release(block);
 }
 CHECK(IDirect3DDevice9_SetTransform(d,D3DTS_WORLD,&c));CHECK(IDirect3DDevice9_Reset(d,pp));observe(d,cycle,"reset",D3DTS_WORLD);
 /* Reset must not be assumed to end backend recording. */
 CHECK(IDirect3DDevice9_BeginStateBlock(d));CHECK(IDirect3DDevice9_SetTransform(d,D3DTS_WORLD,&b));
 CHECK(IDirect3DDevice9_Reset(d,pp));observe(d,cycle,"record-reset-live",D3DTS_WORLD);
 CHECK(IDirect3DDevice9_EndStateBlock(d,&block));REQUIRE(block);CHECK(IDirect3DStateBlock9_Apply(block));observe(d,cycle,"record-reset-apply",D3DTS_WORLD);IDirect3DStateBlock9_Release(block);
 CHECK(IDirect3DDevice9_Clear(d,0,NULL,D3DCLEAR_TARGET,0xff204060,1,0));CHECK(IDirect3DDevice9_Present(d,NULL,NULL,NULL,NULL));return 0;
}
int wmain(int argc,WCHAR **argv)
{
 REQUIRE(argc==2||argc==4);if(argc==4){SetEnvironmentVariableW(L"PW_D3D9_SERVICE64",argv[2]);SetEnvironmentVariableW(L"PW_D3D9_BACKEND64",argv[3]);}
 WNDCLASSW wc={.lpfnWndProc=DefWindowProcW,.hInstance=GetModuleHandleW(NULL),.lpszClassName=L"TransformProxyProof"};REQUIRE(RegisterClassW(&wc));
 HWND w=CreateWindowW(wc.lpszClassName,L"transform",WS_POPUP|WS_VISIBLE,0,0,64,64,NULL,NULL,wc.hInstance,NULL);REQUIRE(w);
 HMODULE dll=LoadLibraryW(argv[1]);REQUIRE(dll);IDirect3D9 *(WINAPI *create)(UINT)=(void *)GetProcAddress(dll,"Direct3DCreate9");REQUIRE(create);
 for(unsigned cycle=0;cycle<3;cycle++){
  IDirect3D9 *f=create(D3D_SDK_VERSION);REQUIRE(f);IDirect3DDevice9 *d=NULL;
  D3DPRESENT_PARAMETERS pp={.Windowed=TRUE,.SwapEffect=D3DSWAPEFFECT_DISCARD,.BackBufferFormat=D3DFMT_A8R8G8B8,.BackBufferWidth=64,.BackBufferHeight=64,.hDeviceWindow=w};
  CHECK(IDirect3D9_CreateDevice(f,0,D3DDEVTYPE_HAL,w,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&d));REQUIRE(!exercise(d,&pp,cycle));
  REQUIRE(IDirect3DDevice9_Release(d)==0);REQUIRE(IDirect3D9_Release(f)==0);
 }
 DestroyWindow(w);FreeLibrary(dll);puts("TRANSFORM_CLOSE status=0");return 0;
}
