/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_native_program.h"
#include <d3d9.h>
struct pw_d3d9_native_program {
 IUnknown *object;
 IDirect3DDevice9 *device;
 uint32_t kind;
};
static uint32_t word(const unsigned char *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
void pw_d3d9_native_program_destroy(struct pw_d3d9_native_program *p)
{
 if(!p)return;
 if(p->object)IUnknown_Release(p->object);
 if(p->device)IDirect3DDevice9_Release(p->device);
 HeapFree(GetProcessHeap(),0,p);
}
void *pw_d3d9_native_program_backend(struct pw_d3d9_native_program *p){return p?p->object:NULL;}
uint32_t pw_d3d9_native_program_kind(struct pw_d3d9_native_program *p){return p?p->kind:0;}
uintptr_t pw_d3d9_native_program_identity(struct pw_d3d9_native_program *p)
{
 IUnknown *identity=NULL;uintptr_t value;
 if(!p||FAILED(IUnknown_QueryInterface(p->object,&IID_IUnknown,(void **)&identity))||!identity)return 0;
 value=(uintptr_t)identity;IUnknown_Release(identity);return value;
}
uint32_t pw_d3d9_native_program_create(void *native_device,uint32_t kind,const void *data,size_t size,
 struct pw_d3d9_native_program **out)
{
 IDirect3DDevice9 *device=native_device;const unsigned char *bytes=data;
 struct pw_d3d9_native_program *p;HRESULT hr;size_t i;
 if(!out)return E_POINTER;
 *out=NULL;
 if(!device||pw_d3d9_program_validate(kind,data,size))return D3DERR_INVALIDCALL;
 p=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*p));if(!p)return E_OUTOFMEMORY;
 p->kind=kind;
 if(kind==PW_D3D9_PROGRAM_DECL){
  D3DVERTEXELEMENT9 elements[65];IDirect3DVertexDeclaration9 *decl=NULL;
  for(i=0;i<size/8;i++){
   elements[i].Stream=bytes[8*i]|(unsigned)bytes[8*i+1]<<8;
   elements[i].Offset=bytes[8*i+2]|(unsigned)bytes[8*i+3]<<8;
   elements[i].Type=bytes[8*i+4];elements[i].Method=bytes[8*i+5];
   elements[i].Usage=bytes[8*i+6];elements[i].UsageIndex=bytes[8*i+7];
  }
  hr=IDirect3DDevice9_CreateVertexDeclaration(device,elements,&decl);p->object=(IUnknown *)decl;
 }else {
  DWORD *tokens=HeapAlloc(GetProcessHeap(),0,size);
  if(!tokens){HeapFree(GetProcessHeap(),0,p);return E_OUTOFMEMORY;}
  for(i=0;i<size/4;i++)tokens[i]=word(bytes+4*i);
  if(kind==PW_D3D9_PROGRAM_VS){IDirect3DVertexShader9 *shader=NULL;hr=IDirect3DDevice9_CreateVertexShader(device,tokens,&shader);p->object=(IUnknown *)shader;}
  else {IDirect3DPixelShader9 *shader=NULL;hr=IDirect3DDevice9_CreatePixelShader(device,tokens,&shader);p->object=(IUnknown *)shader;}
  HeapFree(GetProcessHeap(),0,tokens);
 }
 if(SUCCEEDED(hr)&&!p->object)hr=E_FAIL;
 if(FAILED(hr)){pw_d3d9_native_program_destroy(p);return (uint32_t)hr;}
 p->device=device;IDirect3DDevice9_AddRef(device);*out=p;return (uint32_t)hr;
}
