/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_native_program_query.h"
#include <d3d9.h>
#include <string.h>
static HRESULT query(void *object,uint32_t kind,void *data,UINT *size)
{
 switch(kind){
 case PW_D3D9_PROGRAM_DECL:return IDirect3DVertexDeclaration9_GetDeclaration((IDirect3DVertexDeclaration9 *)object,data,size);
 case PW_D3D9_PROGRAM_VS:return IDirect3DVertexShader9_GetFunction((IDirect3DVertexShader9 *)object,data,size);
 default:return IDirect3DPixelShader9_GetFunction((IDirect3DPixelShader9 *)object,data,size);
 }
}
static void put(unsigned char *p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
void pw_d3d9_native_program_query(struct pw_d3d9_native_program *program,const struct pw_d3d9_program_query_request *q,struct pw_d3d9_program_query_reply *r)
{
 unsigned char validation[24],canonical[65*8];size_t n;void *object,*data=NULL;
 HRESULT hr;UINT units;uint32_t total,available,i,count;D3DVERTEXELEMENT9 elements[65];
 memset(r,0,sizeof(*r));if(!q){r->hresult=D3DERR_INVALIDCALL;return;}
 r->operation=q->operation;r->kind=q->kind;r->size=q->capacity;r->hresult=D3DERR_INVALIDCALL;
 if(pw_d3d9_program_query_encode(validation,sizeof(validation),&n,q)||!program||pw_d3d9_native_program_kind(program)!=q->kind)return;
 object=pw_d3d9_native_program_backend(program);if(!object)return;
 units=q->operation==PW_D3D9_PROGRAM_SIZE?q->capacity:0;
 hr=query(object,q->kind,NULL,&units);
 if(q->operation==PW_D3D9_PROGRAM_SIZE)r->size=units;
 r->hresult=hr;if(FAILED(hr))return;
 if(!units||units>(q->kind==PW_D3D9_PROGRAM_DECL?65:PW_D3D9_PROGRAM_LIMIT)||(q->kind!=PW_D3D9_PROGRAM_DECL&&units%4)){r->hresult=E_NOTIMPL;return;}
 total=q->kind==PW_D3D9_PROGRAM_DECL?units*8:units;
 if(q->operation==PW_D3D9_PROGRAM_SIZE){r->total=total;return;}
 available=q->kind==PW_D3D9_PROGRAM_DECL?total:(q->capacity<total?q->capacity:total);
 if(q->offset>available){r->hresult=D3DERR_INVALIDCALL;return;}
 if(q->kind==PW_D3D9_PROGRAM_DECL){memset(elements,0,sizeof(elements));data=elements;}
 else {data=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,total);if(!data){r->hresult=E_OUTOFMEMORY;return;}}
 units=q->capacity;hr=query(object,q->kind,data,&units);r->hresult=hr;r->size=units;
 if(SUCCEEDED(hr)){
  /* Immutable backend object: size cannot change between SIZE and READ. */
  if(q->kind==PW_D3D9_PROGRAM_DECL&&units!=total/8)r->hresult=E_FAIL;
  else {
   count=available-q->offset;if(count>q->count)count=q->count;
   if(q->kind==PW_D3D9_PROGRAM_DECL){
    for(i=0;i<total/8;i++){
     const D3DVERTEXELEMENT9 *e=elements+i;unsigned char *p=canonical+8*i;
     p[0]=e->Stream;p[1]=e->Stream>>8;p[2]=e->Offset;p[3]=e->Offset>>8;
     p[4]=e->Type;p[5]=e->Method;p[6]=e->Usage;p[7]=e->UsageIndex;
    }
    memcpy(r->data,canonical+q->offset,count);
   }else for(i=0;i<count;i++){
    uint32_t offset=q->offset+i;unsigned char token[4];put(token,((DWORD *)data)[offset/4]);r->data[i]=token[offset%4];
   }
   r->total=total;r->offset=q->offset;r->count=count;
  }
 }
 if(q->kind!=PW_D3D9_PROGRAM_DECL)HeapFree(GetProcessHeap(),0,data);
}
