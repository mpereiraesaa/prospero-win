/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_texture_proxy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static IDirect3DDevice9 parent;
static LONG parent_refs=1;
static unsigned remote_refs[16],releases,failures;
static ULONG WINAPI parent_addref(IDirect3DDevice9 *p)
{assert(p==&parent);return InterlockedIncrement(&parent_refs);}
static ULONG WINAPI parent_release(IDirect3DDevice9 *p)
{assert(p==&parent);return InterlockedDecrement(&parent_refs);}
static HRESULT texture(IDirect3DDevice9 *p,struct pw_d3d9_object_ref ref,
 const struct pw_d3d9_texture_request *q,struct pw_d3d9_texture_reply *r)
{
 assert(p==&parent&&ref.id<16&&remote_refs[ref.id]);
 memset(r,0,sizeof(*r));r->operation=q->operation;r->hresult=S_OK;
 assert(q->operation==PW_D3D9_TEXTURE_DESC);r->levels=1;
 r->desc=(struct pw_d3d9_surface_desc){D3DFMT_A8R8G8B8,D3DRTYPE_SURFACE,D3DUSAGE_RENDERTARGET,D3DPOOL_DEFAULT,0,0,64,64};return S_OK;
}
static HRESULT remote_release(IDirect3DDevice9 *p,struct pw_d3d9_object_ref ref)
{assert(p==&parent&&ref.id<16&&remote_refs[ref.id]);remote_refs[ref.id]--;releases++;return S_OK;}
static HRESULT defer(IDirect3DDevice9 *p,struct pw_d3d9_deferred *n)
{(void)p;(void)n;assert(0);return E_FAIL;}
static void fail(IDirect3DDevice9 *p,HRESULT hr)
{assert(p==&parent&&FAILED(hr));failures++;}
int main(void)
{
 IDirect3DDevice9Vtbl table={0};struct pw_d3d9_texture_proxy_ops ops={texture,remote_release,defer,fail};
 const struct pw_d3d9_object_ref owners[]={{10,7},{11,7}};
 struct pw_d3d9_object_ref zero[2],resolved;UINT count=0;IDirect3DSurface9 *a=NULL,*b=NULL;IUnknown *query=NULL;
 table.AddRef=parent_addref;table.Release=parent_release;parent.lpVtbl=&table;pw_d3d9_texture_proxy_install(&table,&ops);
 assert(pw_d3d9_texture_proxy_owners_install(&parent,owners,2)==S_OK);
 remote_refs[10]=remote_refs[11]=1;
 assert(pw_d3d9_texture_proxy_wrap(&parent,PW_D3D9_KIND_SURFACE,owners[0],1,(void **)&a)==S_OK);
 assert(pw_d3d9_texture_proxy_wrap(&parent,PW_D3D9_KIND_SURFACE,owners[1],1,(void **)&b)==S_OK);
 assert(parent_refs==3);assert(IDirect3DSurface9_Release(a)==0&&parent_refs==2);
 assert(pw_d3d9_texture_proxy_owners_prepare_reset(&parent,zero,2,&count)==S_OK&&count==1&&zero[0].id==10);
 /* This models the callback/concurrent AddRef after the atomic snapshot.
  * It must return promptly without publishing a positive reference. */
 assert(IDirect3DSurface9_AddRef(a)==0);
 assert(FAILED(IDirect3DSurface9_QueryInterface(a,&IID_IUnknown,(void **)&query))&&!query);
 assert(FAILED(pw_d3d9_texture_proxy_resolve(&parent,(IUnknown *)a,PW_D3D9_KIND_SURFACE,&resolved)));
 assert(IDirect3DSurface9_AddRef(b)==2);assert(IDirect3DSurface9_Release(b)==1);
 pw_d3d9_texture_proxy_owners_finish_reset(&parent,TRUE);
 assert(IDirect3DSurface9_AddRef(a)==1&&parent_refs==3);
 assert(pw_d3d9_texture_proxy_resolve(&parent,(IUnknown *)a,PW_D3D9_KIND_SURFACE,&resolved)==S_OK&&resolved.id==10);
 assert(IDirect3DSurface9_Release(a)==0);
 assert(pw_d3d9_texture_proxy_owners_prepare_reset(&parent,zero,2,&count)==S_OK&&count==1&&zero[0].id==10);
 /* B was public-positive in the atomic snapshot. Dropping its last public
  * reference while Reset runs must use normal remote cleanup after retirement. */
 assert(IDirect3DSurface9_Release(b)==0&&parent_refs==1);
 /* Successful native Reset followed by mirror failure still commits retirement:
  * service consumed the zero-public references before the final failed reply. */
 remote_refs[10]=0;
 pw_d3d9_texture_proxy_owners_finish_reset(&parent,FALSE);
 assert(parent_refs==1&&releases==1&&!remote_refs[11]&&!failures);
 /* Populate a new generation after Reset so final close exercises a live
  * public-zero owner shell, not merely an empty owner table. */
 const struct pw_d3d9_object_ref fresh={10,8};remote_refs[10]=1;
 assert(pw_d3d9_texture_proxy_owners_install(&parent,&fresh,1)==S_OK);
 assert(pw_d3d9_texture_proxy_wrap(&parent,PW_D3D9_KIND_SURFACE,fresh,1,(void **)&a)==S_OK);
 assert(IDirect3DSurface9_Release(a)==0&&parent_refs==1);
 /* No old surface dereference after retirement. Closing protects the device
  * with its internal teardown sentinel while the owner table is disposed. */
 assert(pw_d3d9_texture_proxy_parent_release(&parent,&parent_refs)==0&&parent_refs==1);
 assert(IDirect3DSurface9_AddRef(a)==0);
 assert(pw_d3d9_texture_proxy_owners_dispose(&parent)==S_OK);
 assert(!remote_refs[10]&&!remote_refs[11]&&releases==2&&!failures);
 puts("SURFACE_OWNERS_CONTROL frozen_addref=0 early_identity=1 retire=1 parent_cycle=0 status=0");return 0;
}
