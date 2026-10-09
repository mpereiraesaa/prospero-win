/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../wine/ps5/d3d9/pw_d3d9_program_proxy.h"
static LONG parent_refs=1;static unsigned releases,queries,faults,creating_kind;static int nested,defer_failure;
static struct pw_d3d9_deferred *deferred,*retained;static unsigned char storage[PW_D3D9_PROGRAM_LIMIT];static struct pw_d3d9_program_upload upload;
static ULONG WINAPI parent_addref(IDirect3DDevice9 *d){(void)d;return InterlockedIncrement(&parent_refs);}
static ULONG WINAPI parent_release(IDirect3DDevice9 *d){(void)d;return InterlockedDecrement(&parent_refs);}
static HRESULT call(IDirect3DDevice9 *d,const struct pw_d3d9_program_request *q,struct pw_d3d9_program_reply *r)
{
 (void)d;memset(r,0,sizeof(*r));r->operation=q->operation;assert(!pw_d3d9_program_upload_apply(&upload,q,&r->transfer));
 if(q->operation==PW_D3D9_PROGRAM_COMMIT){r->id=q->kind;r->generation=1;creating_kind=q->kind;pw_d3d9_program_upload_finish(&upload);}return S_OK;
}
static HRESULT query(IDirect3DDevice9 *d,struct pw_d3d9_object_ref ref,uint32_t kind,void *out,UINT *size)
{
 (void)d;assert(ref.id==kind&&ref.generation==1);queries++;UINT bytes=kind==7?16:20;
 if(!out)*size=kind==7?2:bytes;else{UINT copied=kind==7?bytes:(*size<bytes?*size:bytes);memcpy(out,storage,copied);if(kind==7)*size=2;}return S_OK;
}
static HRESULT remote_release(IDirect3DDevice9 *d,struct pw_d3d9_object_ref ref)
{(void)d;assert(ref.id>=7&&ref.id<=9&&ref.generation==1);if(nested)return RPC_E_CANTCALLOUT_ININPUTSYNCCALL;releases++;return S_OK;}
static HRESULT defer(IDirect3DDevice9 *d,struct pw_d3d9_deferred *node){(void)d;if(defer_failure){retained=node;return E_OUTOFMEMORY;}assert(!deferred);deferred=node;return S_OK;}
static void fail(IDirect3DDevice9 *d,HRESULT hr){(void)d;(void)hr;faults++;}
static void drain(void)
{struct pw_d3d9_deferred *node=deferred;assert(node);deferred=NULL;node->function(node->context);}
int main(void)
{
 IDirect3DDevice9Vtbl table={.AddRef=parent_addref,.Release=parent_release};IDirect3DDevice9 device={&table};
 struct pw_d3d9_program_proxy_ops ops={call,query,remote_release,defer,fail};pw_d3d9_program_proxy_install(&table,&ops);pw_d3d9_program_upload_init(&upload,storage,sizeof(storage));
 const DWORD code[]={0xfffe0200,0x02000001,0x800f0000,0x90e40000,0x0000ffff};
 IDirect3DVertexShader9 *shader=NULL,*same=NULL;assert(IDirect3DDevice9_CreateVertexShader(&device,(void *)1,&shader)==D3DERR_INVALIDCALL&&!shader);
 assert(IDirect3DDevice9_CreateVertexShader(&device,code,&shader)==S_OK&&shader&&creating_kind==8&&parent_refs==2);
 assert(IDirect3DDevice9_CreateVertexShader(&device,code,&same)==S_OK&&same==shader&&parent_refs==2&&releases==1);
 IUnknown *identity=NULL;assert(IDirect3DVertexShader9_QueryInterface(shader,&IID_IUnknown,(void **)&identity)==S_OK&&identity==(IUnknown *)shader);IUnknown_Release(identity);
 IDirect3DDevice9 *parent=NULL;assert(IDirect3DVertexShader9_GetDevice(shader,&parent)==S_OK&&parent==&device);IDirect3DDevice9_Release(parent);
 UINT bytes=0;assert(IDirect3DVertexShader9_GetFunction(shader,NULL,&bytes)==S_OK&&bytes==sizeof(code));DWORD copied[5]={0};bytes=8;assert(IDirect3DVertexShader9_GetFunction(shader,copied,&bytes)==S_OK&&bytes==8&&!memcmp(copied,code,8));
 struct pw_d3d9_object_ref ref;assert(pw_d3d9_program_proxy_resolve(&device,(void *)1,8,&ref)==D3DERR_INVALIDCALL);
 assert(pw_d3d9_program_proxy_resolve(&device,(IUnknown *)shader,8,&ref)==S_OK&&ref.id==8);assert(pw_d3d9_program_proxy_resolve(&device,(IUnknown *)shader,9,&ref)==D3DERR_INVALIDCALL);
 assert(IDirect3DVertexShader9_Release(same)==1);nested=1;assert(IDirect3DVertexShader9_Release(shader)==0&&deferred&&parent_refs==2);
 drain();assert(deferred&&parent_refs==2&&releases==1);nested=0;drain();assert(parent_refs==1&&releases==2);
 const D3DVERTEXELEMENT9 elements[]={{0,0,D3DDECLTYPE_FLOAT3,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},D3DDECL_END()};
 IDirect3DVertexDeclaration9 *decl=NULL;assert(IDirect3DDevice9_CreateVertexDeclaration(&device,elements,&decl)==S_OK&&decl&&creating_kind==7);
 D3DVERTEXELEMENT9 got[2];UINT count=0;assert(IDirect3DVertexDeclaration9_GetDeclaration(decl,got,&count)==S_OK&&count==2&&!memcmp(got,elements,sizeof(got)));
 assert(IDirect3DVertexDeclaration9_Release(decl)==0&&parent_refs==1&&!faults&&queries==3);
 /* Duplicate cleanup during a pumped callback owns its own retained shell. */
 assert(pw_d3d9_program_proxy_wrap(&device,8,(struct pw_d3d9_object_ref){8,1},(void **)&shader)==S_OK);
 nested=1;
 assert(pw_d3d9_program_proxy_wrap(&device,8,(struct pw_d3d9_object_ref){8,1},(void **)&same)==S_OK&&same==shader&&deferred&&parent_refs==3);
 drain();assert(deferred&&parent_refs==3);nested=0;drain();assert(parent_refs==2);
 assert(IDirect3DVertexShader9_Release(same)==1);assert(!IDirect3DVertexShader9_Release(shader)&&parent_refs==1);
 /* Invalid output still consumes the owned reference through deferred cleanup. */
 nested=1;assert(pw_d3d9_program_proxy_wrap(&device,8,(struct pw_d3d9_object_ref){8,1},NULL)==D3DERR_INVALIDCALL&&deferred&&parent_refs==2);
 nested=0;drain();assert(parent_refs==1);
 /* A failed initial enqueue and a failed retry retain their strong parent. */
 assert(pw_d3d9_program_proxy_wrap(&device,8,(struct pw_d3d9_object_ref){8,1},(void **)&shader)==S_OK);
 nested=1;defer_failure=1;assert(!IDirect3DVertexShader9_Release(shader)&&retained&&parent_refs==2&&faults==1);
 struct pw_d3d9_deferred *saved=retained;retained=NULL;saved->function(saved->context);
 assert(retained==saved&&parent_refs==2&&faults==2);
 defer_failure=0;nested=0;retained=NULL;saved->function(saved->context);assert(parent_refs==1);
 puts("PW_PROGRAM_PROXY shader=1 declaration=1 identity=1 deferred=1 safe_read=1 status=0");return 0;
}
