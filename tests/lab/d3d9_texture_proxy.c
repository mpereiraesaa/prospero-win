/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_texture_proxy.h"
#include "pw_d3d9_staging.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static IDirect3DDevice9 parent;static LONG parent_refs=1;static unsigned refs[16],active[16],calls,writes,failures;static int blocked,negative,fail_operation;static uint64_t generation;
static unsigned char contents[24];static struct pw_d3d9_deferred *pending;
static ULONG WINAPI parent_addref(IDirect3DDevice9 *d){assert(d==&parent);return InterlockedIncrement(&parent_refs);}
static ULONG WINAPI parent_release(IDirect3DDevice9 *d){assert(d==&parent);return InterlockedDecrement(&parent_refs);}
static HRESULT exchange(IDirect3DDevice9 *d,struct pw_d3d9_object_ref ref,const struct pw_d3d9_texture_request *request,struct pw_d3d9_texture_reply *r)
{
 struct pw_d3d9_texture_request decoded;unsigned char wire[PW_D3D9_TEXTURE_MAX_WIRE];size_t bytes;const struct pw_d3d9_texture_request *q=&decoded;unsigned id=ref.id;
 assert(d==&parent);assert(!pw_d3d9_texture_request_encode(wire,sizeof(wire),&bytes,request));assert(!pw_d3d9_texture_request_decode(&decoded,wire,bytes));calls++;
 memset(r,0,sizeof(*r));r->operation=q->operation;r->hresult=S_OK;
 if(q->operation==(unsigned)fail_operation){r->hresult=D3DERR_INVALIDCALL;return D3DERR_INVALIDCALL;}
 if(id)assert(id<16 && ref.generation==9 && refs[id]);
 switch(q->operation){
 case PW_D3D9_TEXTURE_CREATE:id=7;goto created;
 case PW_D3D9_TEXTURE_CREATE_SURFACE:id=9;goto created;
 case PW_D3D9_TEXTURE_CREATE_RT:assert(q->lockable==1);id=10;goto created;
 case PW_D3D9_TEXTURE_CREATE_DEPTH:assert(q->discard==1);id=11;goto created;
 case PW_D3D9_TEXTURE_SURFACE_LEVEL:assert(ref.id==7 && q->level==2);id=8;
 created:refs[id]++;r->object=(struct pw_d3d9_object_ref){id,9};r->levels=id==7?6:1;break;
 case PW_D3D9_TEXTURE_DESC:r->levels=id==7?6:1;break;
 case PW_D3D9_TEXTURE_LOCK:assert(!active[id]);active[id]=1;generation++;r->lock_generation=generation;r->pitch=negative?-16:16;r->rows=2;r->row_bytes=8;r->length=24;break;
 case PW_D3D9_TEXTURE_READ:assert(active[id] && q->lock_generation==generation && !q->offset && q->count==24);r->lock_generation=generation;r->count=24;memcpy(r->data,contents,24);break;
 case PW_D3D9_TEXTURE_WRITE:assert(active[id] && q->lock_generation==generation && !q->offset && q->count==24);memcpy(contents,q->data,24);writes++;break;
 case PW_D3D9_TEXTURE_UNLOCK:case PW_D3D9_TEXTURE_CANCEL_LOCK:assert(active[id]);active[id]=0;break;
 case PW_D3D9_TEXTURE_DIRTY:assert(id==7 && q->has_rect && q->left==1 && q->top==2 && q->right==3 && q->bottom==4);break;
 case PW_D3D9_TEXTURE_UPDATE:assert(!id && q->source.id==7 && q->destination.id==7);break;
 case PW_D3D9_TEXTURE_UPDATE_SURFACE:assert(!id && q->source.id==8 && q->destination.id==9 && q->has_point && q->x==3 && q->y==4);break;
 case PW_D3D9_TEXTURE_COLOR_FILL:assert(id==10 && q->color==0xff123456 && q->has_rect);break;
 case PW_D3D9_TEXTURE_RT_DATA:assert(!id && q->source.id==10 && q->destination.id==9);break;
 case PW_D3D9_TEXTURE_STRETCH:assert(!id && q->source.id==8 && q->destination.id==10 && q->filter==D3DTEXF_LINEAR && q->has_destination_rect && q->destination_left==1);break;
 default:assert(0);
 }
 r->desc=(struct pw_d3d9_surface_desc){D3DFMT_A8R8G8B8,D3DRTYPE_SURFACE,0,D3DPOOL_SYSTEMMEM,0,0,16,8};
 assert(!pw_d3d9_texture_reply_encode(wire,sizeof(wire),&bytes,r));assert(!pw_d3d9_texture_reply_decode(r,wire,bytes));return S_OK;
}
static HRESULT remote_release(IDirect3DDevice9 *d,struct pw_d3d9_object_ref r)
{assert(d==&parent && r.id<16 && refs[r.id] && r.generation==9);if(blocked)return RPC_E_CANTCALLOUT_ININPUTSYNCCALL;if(!--refs[r.id])active[r.id]=0;return S_OK;}
static HRESULT defer(IDirect3DDevice9 *d,struct pw_d3d9_deferred *n){assert(d==&parent && !pending);pending=n;return S_OK;}
static void fail(IDirect3DDevice9 *d,HRESULT hr){assert(d==&parent && FAILED(hr));failures++;}
int main(void)
{
 IDirect3DDevice9Vtbl table={0};struct pw_d3d9_texture_proxy_ops ops={exchange,remote_release,defer,fail};IDirect3DTexture9 *t,*alias;IDirect3DSurface9 *s,*off,*rt,*depth;IDirect3DDevice9 *owner;IUnknown *u;D3DSURFACE_DESC desc;D3DLOCKED_RECT lock;struct pw_d3d9_object_ref resolved={0};RECT rect={1,2,3,4};POINT point={3,4};
 table.AddRef=parent_addref;table.Release=parent_release;parent.lpVtbl=&table;pw_d3d9_texture_proxy_install(&table,&ops);
 assert(!table.Present && !table.GetRenderState);
 assert(IDirect3DDevice9_CreateTexture(&parent,32,32,0,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,NULL,NULL)==D3DERR_INVALIDCALL && !calls);
 assert(IDirect3DDevice9_CreateTexture(&parent,32,32,0,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&t,NULL)==S_OK);
 assert(IDirect3DTexture9_GetLevelCount(t)==6 && IDirect3DTexture9_GetType(t)==D3DRTYPE_TEXTURE);
 assert(IDirect3DTexture9_QueryInterface(t,&IID_IDirect3DBaseTexture9,(void **)&u)==S_OK && u==(IUnknown *)t);IUnknown_Release(u);
 assert(IDirect3DTexture9_QueryInterface(t,&IID_IDirect3DSurface9,(void **)&u)==E_NOINTERFACE && !u);
 assert(IDirect3DTexture9_GetDevice(t,&owner)==S_OK && owner==&parent);IDirect3DDevice9_Release(owner);
 assert(pw_d3d9_texture_proxy_resolve(&parent,(IUnknown *)(uintptr_t)1,PW_D3D9_KIND_TEXTURE_2D,&resolved)==D3DERR_INVALIDCALL);
 assert(pw_d3d9_texture_proxy_resolve(&parent,(IUnknown *)t,PW_D3D9_KIND_SURFACE,&resolved)==D3DERR_INVALIDCALL);
 assert(pw_d3d9_texture_proxy_resolve(&parent,(IUnknown *)t,PW_D3D9_KIND_TEXTURE_2D,&resolved)==S_OK && resolved.id==7);
 refs[7]++;assert(pw_d3d9_texture_proxy_wrap(&parent,PW_D3D9_KIND_TEXTURE_2D,resolved,0,(void **)&alias)==S_OK && alias==t && refs[7]==1);IDirect3DTexture9_Release(alias);
 assert(IDirect3DTexture9_GetLevelDesc(t,0,&desc)==S_OK && desc.Width==16 && desc.Height==8);
 assert(IDirect3DTexture9_GetSurfaceLevel(t,2,&s)==S_OK && IDirect3DSurface9_GetType(s)==D3DRTYPE_SURFACE);
 assert(IDirect3DSurface9_GetDesc(s,&desc)==S_OK && desc.Format==D3DFMT_A8R8G8B8);
 if(sizeof(void *)==4)for(negative=0;negative<2;negative++){
  memset(contents,0x33,sizeof(contents));writes=0;
  assert(IDirect3DTexture9_LockRect(t,2,&lock,NULL,0)==S_OK && lock.Pitch==(negative?-16:16) && (uintptr_t)lock.pBits<=UINT32_MAX);
  ((unsigned char *)lock.pBits)[2]=0x77;
  assert(IDirect3DTexture9_UnlockRect(t,1)==D3DERR_INVALIDCALL && active[7]);
  assert(IDirect3DTexture9_UnlockRect(t,2)==S_OK && contents[(negative?16:0)+2]==0x77 && writes==1 && !pw_d3d9_staging_bytes());
  assert(IDirect3DSurface9_LockRect(s,&lock,NULL,D3DLOCK_READONLY)==S_OK);assert(IDirect3DSurface9_UnlockRect(s)==S_OK && writes==1);
 }
 fail_operation=PW_D3D9_TEXTURE_DESC;memset(&desc,0xa5,sizeof(desc));assert(IDirect3DTexture9_GetLevelDesc(t,0,&desc)==D3DERR_INVALIDCALL && desc.Width==0xa5a5a5a5);fail_operation=0;
 assert(IDirect3DTexture9_AddDirtyRect(t,&rect)==S_OK);
 {DWORD data=0x12345678,read=0,n=4;assert(IDirect3DTexture9_SetPrivateData(t,&IID_IUnknown,&data,4,0)==S_OK);assert(IDirect3DTexture9_GetPrivateData(t,&IID_IUnknown,&read,&n)==S_OK && read==data);assert(IDirect3DTexture9_FreePrivateData(t,&IID_IUnknown)==S_OK);}
 assert(IDirect3DDevice9_CreateOffscreenPlainSurface(&parent,16,8,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&off,NULL)==S_OK);
 assert(IDirect3DDevice9_CreateRenderTarget(&parent,16,8,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,-1,&rt,NULL)==S_OK);
 assert(IDirect3DDevice9_CreateDepthStencilSurface(&parent,16,8,D3DFMT_D16,D3DMULTISAMPLE_NONE,0,-1,&depth,NULL)==S_OK);
 assert(IDirect3DDevice9_UpdateTexture(&parent,(IDirect3DBaseTexture9 *)t,(IDirect3DBaseTexture9 *)t)==S_OK);
 assert(IDirect3DDevice9_UpdateSurface(&parent,s,&rect,off,&point)==S_OK);
 assert(IDirect3DDevice9_StretchRect(&parent,s,&rect,rt,&rect,D3DTEXF_LINEAR)==S_OK);
 assert(IDirect3DDevice9_UpdateSurface(&parent,(void *)(uintptr_t)1,NULL,off,NULL)==D3DERR_INVALIDCALL);
 assert(IDirect3DDevice9_ColorFill(&parent,rt,&rect,0xff123456)==S_OK);assert(IDirect3DDevice9_GetRenderTargetData(&parent,rt,off)==S_OK);
 IDirect3DSurface9_Release(off);IDirect3DSurface9_Release(rt);IDirect3DSurface9_Release(depth);IDirect3DSurface9_Release(s);
 if(sizeof(void *)==4)assert(IDirect3DTexture9_LockRect(t,0,&lock,NULL,0)==S_OK && pw_d3d9_staging_bytes()==24);
 else assert(IDirect3DTexture9_LockRect(t,0,&lock,NULL,0)==E_OUTOFMEMORY && !active[7]);
 blocked=1;
 assert(IDirect3DTexture9_Release(t)==0 && pending && active[7]==(sizeof(void *)==4) && pw_d3d9_staging_bytes()==(sizeof(void *)==4?24:0) && parent_refs==2);
 {struct pw_d3d9_deferred *n=pending;pending=NULL;blocked=0;n->function(n->context);}assert(!active[7] && !pw_d3d9_staging_bytes() && parent_refs==1);
 for(unsigned i=0;i<16;i++)assert(!refs[i]);
 assert(!failures);
 printf("PASS texture proxy: typed2D/surface, real signed low32 staging, canonical identity, RT/depth/stretch and deferred locked retirement calls=%u\n",calls);return 0;
}
