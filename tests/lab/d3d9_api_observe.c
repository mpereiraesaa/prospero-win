/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "../../wine/ps5/d3d9/pw_d3d9_api_observe.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
static unsigned calls,logs;
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
int main(int argc,char **argv)
{
 (void)argv;enabled=argc>1;SetEnvironmentVariableA("PW_D3D9_DIAGNOSTICS",enabled?"1":"0");
 static const IDirect3DDevice9Vtbl raw={.QueryInterface=query,.ValidateDevice=validate,.DrawIndexedPrimitiveUP=draw,.SetCursorPosition=position,.ShowCursor=show,.GetNPatchMode=patches,.TestCooperativeLevel=destroyed};
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
 assert(calls==12&&logs==EXPECT_LOGS(8u));printf("PW_API_OBSERVE enabled=%d calls=%u logs=%u abi=1 reentry=1 invalid_iid=1 destroyed_self=1 status=0\n",enabled,calls,logs);return 0;
}
