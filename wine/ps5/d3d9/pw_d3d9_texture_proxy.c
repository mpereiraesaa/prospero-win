/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_texture_proxy.h"
#include "pw_d3d9_api_observe.h"
#include "pw_d3d9_private_data.h"
struct proxy {
 IUnknown iface;ULONG refs;uint32_t kind,levels,lock_level;LONG busy;IDirect3DDevice9 *parent;
 struct pw_d3d9_private_data private_data;struct pw_d3d9_texture_client client;struct pw_d3d9_deferred cleanup;struct proxy *next;
};
static struct pw_d3d9_texture_proxy_ops ops;
static SRWLOCK cache_lock=SRWLOCK_INIT;
static struct proxy *cache;
static IDirect3DTexture9Vtbl texture_vtable;
static IDirect3DSurface9Vtbl surface_vtable;
static struct proxy *impl(void *iface){return iface;}
static ULONG addref(void *iface){struct proxy *p=impl(iface);ULONG n;AcquireSRWLockExclusive(&cache_lock);n=++p->refs;ReleaseSRWLockExclusive(&cache_lock);return n;}
static void free_local(struct proxy *p)
{
 IDirect3DDevice9 *parent=p->parent;
 /* Native retirement/session cancellation already owns the backend unlock. */
 p->client.generation=0;pw_d3d9_texture_client_cancel(&p->client);
 pw_d3d9_private_dispose(&p->private_data);
 HeapFree(GetProcessHeap(),0,p);IDirect3DDevice9_Release(parent);
}
static void finish(void *context)
{
 struct proxy *p=context;HRESULT hr=ops.release(p->parent,p->client.object);
 if(hr==RPC_E_CANTCALLOUT_ININPUTSYNCCALL){p->cleanup.function=finish;p->cleanup.context=p;hr=ops.defer(p->parent,&p->cleanup);if(FAILED(hr))ops.fail(p->parent,hr);return;}
 if(FAILED(hr))ops.fail(p->parent,hr);
 free_local(p);
}
static ULONG release(void *iface)
{
 struct proxy *p=impl(iface);ULONG n;AcquireSRWLockExclusive(&cache_lock);n=--p->refs;
 if(!n){struct proxy **link=&cache;while(*link && *link!=p)link=&(*link)->next;if(*link)*link=p->next;}
 ReleaseSRWLockExclusive(&cache_lock);if(!n)finish(p);return n;
}
static HRESULT query(void *iface,REFIID iid,void **out)
{
 struct proxy *p=impl(iface);int match;
 if(!out)return E_POINTER;
 *out=NULL;if(!iid)return E_NOINTERFACE;
 match=IsEqualGUID(iid,&IID_IUnknown)||IsEqualGUID(iid,&IID_IDirect3DResource9);
 if(p->kind==PW_D3D9_KIND_TEXTURE_2D)match|=IsEqualGUID(iid,&IID_IDirect3DBaseTexture9)||IsEqualGUID(iid,&IID_IDirect3DTexture9);
 else match|=IsEqualGUID(iid,&IID_IDirect3DSurface9);
 if(!match)return E_NOINTERFACE;
 addref(iface);*out=iface;return S_OK;
}
static HRESULT get_device(void *iface,IDirect3DDevice9 **out)
{if(!out)return D3DERR_INVALIDCALL;*out=impl(iface)->parent;IDirect3DDevice9_AddRef(*out);return S_OK;}
static HRESULT unsupported(void *iface){ops.fail(impl(iface)->parent,E_NOTIMPL);return E_NOTIMPL;}
static uint32_t client_call(void *context,struct pw_d3d9_object_ref ref,const struct pw_d3d9_texture_request *q,struct pw_d3d9_texture_reply *r)
{return ops.texture(((struct proxy *)context)->parent,ref,q,r);}
static void client_fail(void *context,uint32_t hr){ops.fail(((struct proxy *)context)->parent,(HRESULT)hr);}
static HRESULT invoke(struct proxy *p,const struct pw_d3d9_texture_request *q,struct pw_d3d9_texture_reply *r)
{
 HRESULT hr=ops.texture(p->parent,p->client.object,q,r);
 if(SUCCEEDED(hr) && (r->operation!=q->operation || r->hresult!=(uint32_t)hr)){ops.fail(p->parent,E_FAIL);return E_FAIL;}
 return hr;
}
static HRESULT adopt(struct proxy *p,uint32_t levels,void **out)
{
 struct proxy *found=NULL;HRESULT hr=S_OK;
 if(!levels){struct pw_d3d9_texture_request q={0};struct pw_d3d9_texture_reply r={0};q.operation=PW_D3D9_TEXTURE_DESC;hr=invoke(p,&q,&r);if(SUCCEEDED(hr))levels=r.levels;}
 if(FAILED(hr) || !levels || (p->kind==PW_D3D9_KIND_SURFACE && levels!=1)){
  if(SUCCEEDED(hr)){ops.fail(p->parent,E_FAIL);hr=E_FAIL;}finish(p);return hr;
 }
 p->levels=levels;AcquireSRWLockExclusive(&cache_lock);
 for(struct proxy *it=cache;it;it=it->next)if(it->parent==p->parent && it->kind==p->kind && it->client.object.id==p->client.object.id && it->client.object.generation==p->client.object.generation){found=it;found->refs++;break;}
 if(!found){p->next=cache;cache=p;}
 ReleaseSRWLockExclusive(&cache_lock);
 if(found){*out=&found->iface;finish(p);}else *out=&p->iface;
 return hr;
}
static struct proxy *allocate(IDirect3DDevice9 *parent,uint32_t kind)
{
 struct proxy *p=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*p));if(!p)return NULL;
 p->iface.lpVtbl=(IUnknownVtbl *)(kind==PW_D3D9_KIND_TEXTURE_2D?
  (const void *)PW_D3D9_API_OBSERVE(IDirect3DTexture9,&texture_vtable):
  (const void *)PW_D3D9_API_OBSERVE(IDirect3DSurface9,&surface_vtable));
 if(!p->iface.lpVtbl){HeapFree(GetProcessHeap(),0,p);return NULL;}
 p->parent=parent;p->kind=kind;p->refs=1;p->client.context=p;p->client.call=client_call;p->client.fail=client_fail;IDirect3DDevice9_AddRef(parent);return p;
}
HRESULT pw_d3d9_texture_proxy_wrap(IDirect3DDevice9 *parent,uint32_t kind,struct pw_d3d9_object_ref ref,uint32_t levels,void **out)
{
 struct proxy *p;
 if(out)*out=NULL;
 IDirect3DDevice9_AddRef(parent);
 if(!out || !ref.id || !ref.generation || (kind!=PW_D3D9_KIND_TEXTURE_2D && kind!=PW_D3D9_KIND_SURFACE)){
  ops.fail(parent,D3DERR_INVALIDCALL);IDirect3DDevice9_Release(parent);return D3DERR_INVALIDCALL;
 }
 p=allocate(parent,kind);
 if(!p){ops.fail(parent,E_OUTOFMEMORY);IDirect3DDevice9_Release(parent);return E_OUTOFMEMORY;}
 p->client.object=ref;IDirect3DDevice9_Release(parent);return adopt(p,levels,out);
}
HRESULT pw_d3d9_texture_proxy_resolve(IDirect3DDevice9 *parent,IUnknown *local,uint32_t kind,struct pw_d3d9_object_ref *out)
{
 HRESULT hr=D3DERR_INVALIDCALL;if(!out)return hr;
 AcquireSRWLockExclusive(&cache_lock);
 for(struct proxy *p=cache;p;p=p->next)if(&p->iface==local && p->parent==parent && p->kind==kind){*out=p->client.object;hr=S_OK;break;}
 ReleaseSRWLockExclusive(&cache_lock);return hr;
}
static void describe(D3DSURFACE_DESC *out,const struct pw_d3d9_surface_desc *d)
{out->Format=d->format;out->Type=d->type;out->Usage=d->usage;out->Pool=d->pool;out->MultiSampleType=d->multisample_type;out->MultiSampleQuality=d->multisample_quality;out->Width=d->width;out->Height=d->height;}
static HRESULT get_desc(void *iface,UINT level,D3DSURFACE_DESC *out)
{
 struct proxy *p=impl(iface);struct pw_d3d9_texture_request q={0};struct pw_d3d9_texture_reply r={0};HRESULT hr;
 if(!out)return D3DERR_INVALIDCALL;
 addref(iface);q.operation=PW_D3D9_TEXTURE_DESC;q.level=level;hr=invoke(p,&q,&r);if(SUCCEEDED(hr))describe(out,&r.desc);release(iface);return hr;
}
static void rect(struct pw_d3d9_texture_request *q,const RECT *r)
{if(r){q->has_rect=1;q->left=r->left;q->top=r->top;q->right=r->right;q->bottom=r->bottom;}}
static HRESULT lock_rect(void *iface,UINT level,D3DLOCKED_RECT *out,const RECT *r,DWORD flags)
{
 struct proxy *p=impl(iface);struct pw_d3d9_texture_request q={0};D3DLOCKED_RECT locked={0};HRESULT hr;
 if(!out)return D3DERR_INVALIDCALL;
 addref(iface);if(InterlockedCompareExchange(&p->busy,1,0)){release(iface);return D3DERR_INVALIDCALL;}
 q.operation=PW_D3D9_TEXTURE_LOCK;q.level=level;q.flags=flags;rect(&q,r);
 hr=pw_d3d9_texture_client_lock(&p->client,&q,(int32_t *)&locked.Pitch,&locked.pBits);
 if(SUCCEEDED(hr)){*out=locked;p->lock_level=level;}
 InterlockedExchange(&p->busy,0);release(iface);return hr;
}
static HRESULT unlock_rect(void *iface,UINT level)
{
 struct proxy *p=impl(iface);HRESULT hr;
 addref(iface);if(InterlockedCompareExchange(&p->busy,1,0)){release(iface);return D3DERR_INVALIDCALL;}
 hr=p->client.generation && level!=p->lock_level?D3DERR_INVALIDCALL:(HRESULT)pw_d3d9_texture_client_unlock(&p->client);InterlockedExchange(&p->busy,0);release(iface);return hr;
}
static HRESULT hint(void *iface,uint32_t operation,uint32_t value,uint32_t *out,int sticky)
{
 struct proxy *p=impl(iface);struct pw_d3d9_texture_request q={0};struct pw_d3d9_texture_reply r={0};HRESULT hr;
 addref(iface);q.operation=operation;q.value=value;hr=invoke(p,&q,&r);
 if(SUCCEEDED(hr)){if(out)*out=r.value;}else if(sticky)ops.fail(p->parent,hr);
 release(iface);return hr;
}
#define COMMON(tag,type) \
static HRESULT WINAPI tag##_query(type *s,REFIID i,void **o){return query(s,i,o);} \
static ULONG WINAPI tag##_addref(type *s){return addref(s);} \
static ULONG WINAPI tag##_release(type *s){return release(s);} \
static HRESULT WINAPI tag##_device(type *s,IDirect3DDevice9 **o){return get_device(s,o);} \
static HRESULT WINAPI tag##_setprivate(type *s,REFGUID g,const void *d,DWORD n,DWORD f){HRESULT hr;addref(s);hr=pw_d3d9_private_set(&impl(s)->private_data,g,d,n,f);release(s);return hr;} \
static HRESULT WINAPI tag##_getprivate(type *s,REFGUID g,void *d,DWORD *n){HRESULT hr;addref(s);hr=pw_d3d9_private_get(&impl(s)->private_data,g,d,n);release(s);return hr;} \
static HRESULT WINAPI tag##_freeprivate(type *s,REFGUID g){HRESULT hr;addref(s);hr=pw_d3d9_private_free(&impl(s)->private_data,g);release(s);return hr;} \
static DWORD WINAPI tag##_setpriority(type *s,DWORD n){uint32_t value=0;hint(s,PW_D3D9_TEXTURE_SET_PRIORITY,n,&value,1);return value;} \
static DWORD WINAPI tag##_priority(type *s){uint32_t value=0;hint(s,PW_D3D9_TEXTURE_GET_PRIORITY,0,&value,1);return value;} \
static void WINAPI tag##_preload(type *s){hint(s,PW_D3D9_TEXTURE_PRELOAD,0,NULL,1);} \
static D3DRESOURCETYPE WINAPI tag##_type(type *s){return impl(s)->kind==PW_D3D9_KIND_TEXTURE_2D?D3DRTYPE_TEXTURE:D3DRTYPE_SURFACE;}
COMMON(texture,IDirect3DTexture9)
COMMON(surface,IDirect3DSurface9)
static DWORD WINAPI texture_setlod(IDirect3DTexture9 *s,DWORD n){uint32_t value=0;hint(s,PW_D3D9_TEXTURE_SET_LOD,n,&value,1);return value;}
static DWORD WINAPI texture_lod(IDirect3DTexture9 *s){uint32_t value=0;hint(s,PW_D3D9_TEXTURE_GET_LOD,0,&value,1);return value;}
static DWORD WINAPI texture_levels(IDirect3DTexture9 *s){return impl(s)->levels;}
static HRESULT WINAPI texture_setfilter(IDirect3DTexture9 *s,D3DTEXTUREFILTERTYPE f){return hint(s,PW_D3D9_TEXTURE_SET_AUTOGEN_FILTER,(uint32_t)f,NULL,0);}
static D3DTEXTUREFILTERTYPE WINAPI texture_filter(IDirect3DTexture9 *s){uint32_t value=0;hint(s,PW_D3D9_TEXTURE_GET_AUTOGEN_FILTER,0,&value,1);return value;}
static void WINAPI texture_generate(IDirect3DTexture9 *s){hint(s,PW_D3D9_TEXTURE_GENERATE_MIPS,0,NULL,1);}
static HRESULT WINAPI texture_desc(IDirect3DTexture9 *s,UINT l,D3DSURFACE_DESC *d){return get_desc(s,l,d);}
static HRESULT WINAPI surface_desc(IDirect3DSurface9 *s,D3DSURFACE_DESC *d){return get_desc(s,0,d);}
static HRESULT WINAPI texture_lock(IDirect3DTexture9 *s,UINT l,D3DLOCKED_RECT *d,const RECT *r,DWORD f){return lock_rect(s,l,d,r,f);}
static HRESULT WINAPI surface_lock(IDirect3DSurface9 *s,D3DLOCKED_RECT *d,const RECT *r,DWORD f){return lock_rect(s,0,d,r,f);}
static HRESULT WINAPI texture_unlock(IDirect3DTexture9 *s,UINT l){return unlock_rect(s,l);}
static HRESULT WINAPI surface_unlock(IDirect3DSurface9 *s){return unlock_rect(s,0);}
static HRESULT WINAPI texture_dirty(IDirect3DTexture9 *s,const RECT *r)
{struct pw_d3d9_texture_request q={0};struct pw_d3d9_texture_reply reply={0};HRESULT hr;addref(s);q.operation=PW_D3D9_TEXTURE_DIRTY;rect(&q,r);hr=invoke(impl(s),&q,&reply);release(s);return hr;}
static HRESULT WINAPI texture_surface(IDirect3DTexture9 *s,UINT level,IDirect3DSurface9 **out)
{
 struct proxy *parent=impl(s),*p;struct pw_d3d9_texture_request q={0};struct pw_d3d9_texture_reply r={0};HRESULT hr;
 if(!out)return D3DERR_INVALIDCALL;
 *out=NULL;addref(s);p=allocate(parent->parent,PW_D3D9_KIND_SURFACE);if(!p){release(s);return E_OUTOFMEMORY;}
 q.operation=PW_D3D9_TEXTURE_SURFACE_LEVEL;q.level=level;hr=invoke(parent,&q,&r);
 if(SUCCEEDED(hr)){p->client.object=r.object;if(!r.object.id || !r.object.generation){ops.fail(p->parent,E_FAIL);free_local(p);hr=E_FAIL;}else {HRESULT adopted=adopt(p,r.levels,(void **)out);if(FAILED(adopted))hr=adopted;}}else free_local(p);
 release(s);return hr;
}
static HRESULT WINAPI surface_container(IDirect3DSurface9 *s,REFIID iid,void **out)
{
 static const IID *const interfaces[]={NULL,&IID_IUnknown,&IID_IDirect3DDevice9,&IID_IDirect3DResource9,
  &IID_IDirect3DBaseTexture9,&IID_IDirect3DTexture9,&IID_IDirect3DSwapChain9};
 struct proxy *surface=impl(s),*holder;struct pw_d3d9_texture_request q={0};struct pw_d3d9_texture_reply r={0};
 IUnknown *texture=NULL;HRESULT hr,local;unsigned selector;
 if(!out)return D3DERR_INVALIDCALL;
 *out=NULL;if(!iid)return E_NOINTERFACE;
 for(selector=1;selector<sizeof(interfaces)/sizeof(*interfaces);selector++)if(IsEqualGUID(iid,interfaces[selector]))break;
 if(selector==sizeof(interfaces)/sizeof(*interfaces))return E_NOINTERFACE;
 addref(s);
 /* Allocate retirement storage before obtaining any owned remote reference. */
 holder=allocate(surface->parent,PW_D3D9_KIND_TEXTURE_2D);
 if(!holder){release(s);return E_OUTOFMEMORY;}
 q.operation=PW_D3D9_TEXTURE_CONTAINER;q.value=selector;hr=invoke(surface,&q,&r);
 if(FAILED(hr)){free_local(holder);goto done;}
 if(!r.object.id || !r.object.generation ||
    (r.container_kind!=PW_D3D9_KIND_DEVICE && r.container_kind!=PW_D3D9_KIND_TEXTURE_2D) ||
    (r.container_kind==PW_D3D9_KIND_DEVICE ? r.levels!=0 : !r.levels)){
  ops.fail(surface->parent,E_FAIL);free_local(holder);hr=E_FAIL;goto done;
 }
 holder->client.object=r.object;
 if(r.container_kind==PW_D3D9_KIND_DEVICE){
  /* Session adapter verifies this ID is the exact parent device before returning it. */
  finish(holder);local=IDirect3DDevice9_QueryInterface(surface->parent,iid,out);
 }else{
  local=adopt(holder,r.levels,(void **)&texture);
  if(SUCCEEDED(local)){local=IUnknown_QueryInterface(texture,iid,out);IUnknown_Release(texture);}
 }
 if(FAILED(local))hr=local;
 done:release(s);return hr;
}
static HRESULT WINAPI surface_getdc(IDirect3DSurface9 *s,HDC *dc){(void)dc;return unsupported(s);}
static HRESULT WINAPI surface_releasedc(IDirect3DSurface9 *s,HDC dc){(void)dc;return unsupported(s);}
#define COMMON_SLOTS(t) t##_query,t##_addref,t##_release,t##_device,t##_setprivate,t##_getprivate,t##_freeprivate,t##_setpriority,t##_priority,t##_preload,t##_type
static IDirect3DTexture9Vtbl texture_vtable={COMMON_SLOTS(texture),texture_setlod,texture_lod,texture_levels,texture_setfilter,texture_filter,texture_generate,texture_desc,texture_surface,texture_lock,texture_unlock,texture_dirty};
static IDirect3DSurface9Vtbl surface_vtable={COMMON_SLOTS(surface),surface_container,surface_desc,surface_lock,surface_unlock,surface_getdc,surface_releasedc};
static HRESULT create_resource(IDirect3DDevice9 *device,uint32_t kind,const struct pw_d3d9_texture_request *q,void **out,HANDLE *shared)
{
 struct proxy *p;struct pw_d3d9_texture_reply r={0};HRESULT hr,adopt_hr;
 if(!out)return D3DERR_INVALIDCALL;
 *out=NULL;if(shared)return D3DERR_INVALIDCALL;
 p=allocate(device,kind);if(!p)return E_OUTOFMEMORY;
 hr=ops.texture(device,(struct pw_d3d9_object_ref){0},q,&r);
 if(FAILED(hr)){free_local(p);return hr;}
 if(r.operation!=q->operation || r.hresult!=(uint32_t)hr || !r.object.id || !r.object.generation){ops.fail(device,E_FAIL);free_local(p);return E_FAIL;}
 p->client.object=r.object;adopt_hr=adopt(p,r.levels,out);return FAILED(adopt_hr)?adopt_hr:hr;
}
static HRESULT WINAPI create_texture(IDirect3DDevice9 *d,UINT w,UINT h,UINT levels,DWORD usage,D3DFORMAT format,D3DPOOL pool,IDirect3DTexture9 **out,HANDLE *shared)
{struct pw_d3d9_texture_request q={0};q.operation=PW_D3D9_TEXTURE_CREATE;q.width=w;q.height=h;q.levels=levels;q.usage=usage;q.format=format;q.pool=pool;return create_resource(d,PW_D3D9_KIND_TEXTURE_2D,&q,(void **)out,shared);}
static HRESULT WINAPI create_surface(IDirect3DDevice9 *d,UINT w,UINT h,D3DFORMAT format,D3DPOOL pool,IDirect3DSurface9 **out,HANDLE *shared)
{struct pw_d3d9_texture_request q={0};q.operation=PW_D3D9_TEXTURE_CREATE_SURFACE;q.width=w;q.height=h;q.format=format;q.pool=pool;return create_resource(d,PW_D3D9_KIND_SURFACE,&q,(void **)out,shared);}
static HRESULT WINAPI create_rt(IDirect3DDevice9 *d,UINT w,UINT h,D3DFORMAT format,D3DMULTISAMPLE_TYPE sample,DWORD quality,BOOL lockable,IDirect3DSurface9 **out,HANDLE *shared)
{struct pw_d3d9_texture_request q={0};q.operation=PW_D3D9_TEXTURE_CREATE_RT;q.width=w;q.height=h;q.format=format;q.multisample_type=sample;q.multisample_quality=quality;q.lockable=(uint32_t)lockable;return create_resource(d,PW_D3D9_KIND_SURFACE,&q,(void **)out,shared);}
static HRESULT WINAPI create_depth(IDirect3DDevice9 *d,UINT w,UINT h,D3DFORMAT format,D3DMULTISAMPLE_TYPE sample,DWORD quality,BOOL discard,IDirect3DSurface9 **out,HANDLE *shared)
{struct pw_d3d9_texture_request q={0};q.operation=PW_D3D9_TEXTURE_CREATE_DEPTH;q.width=w;q.height=h;q.format=format;q.multisample_type=sample;q.multisample_quality=quality;q.discard=(uint32_t)discard;return create_resource(d,PW_D3D9_KIND_SURFACE,&q,(void **)out,shared);}
/* Resolve and pin both objects atomically before crossing into a callback. */
static HRESULT copy_resources(IDirect3DDevice9 *device,IUnknown *source,IUnknown *destination,uint32_t kind,struct pw_d3d9_texture_request *q)
{
 struct proxy *src=NULL,*dst=NULL;struct pw_d3d9_texture_reply r={0};HRESULT hr;
 AcquireSRWLockExclusive(&cache_lock);
 for(struct proxy *p=cache;p;p=p->next)if(p->parent==device && p->kind==kind){if(&p->iface==source)src=p;if(&p->iface==destination)dst=p;}
 if(src && dst){src->refs++;dst->refs++;q->source=src->client.object;q->destination=dst->client.object;}
 ReleaseSRWLockExclusive(&cache_lock);
 if(!src || !dst)return D3DERR_INVALIDCALL;
 hr=ops.texture(device,(struct pw_d3d9_object_ref){0},q,&r);
 if(SUCCEEDED(hr) && (r.operation!=q->operation || r.hresult!=(uint32_t)hr)){ops.fail(device,E_FAIL);hr=E_FAIL;}
 release(src);release(dst);return hr;
}
static HRESULT WINAPI update_texture(IDirect3DDevice9 *d,IDirect3DBaseTexture9 *src,IDirect3DBaseTexture9 *dst)
{struct pw_d3d9_texture_request q={0};q.operation=PW_D3D9_TEXTURE_UPDATE;return copy_resources(d,(IUnknown *)src,(IUnknown *)dst,PW_D3D9_KIND_TEXTURE_2D,&q);}
static HRESULT WINAPI update_surface(IDirect3DDevice9 *d,IDirect3DSurface9 *src,const RECT *r,IDirect3DSurface9 *dst,const POINT *point)
{struct pw_d3d9_texture_request q={0};q.operation=PW_D3D9_TEXTURE_UPDATE_SURFACE;rect(&q,r);if(point){q.has_point=1;q.x=point->x;q.y=point->y;}return copy_resources(d,(IUnknown *)src,(IUnknown *)dst,PW_D3D9_KIND_SURFACE,&q);}
static HRESULT WINAPI stretch(IDirect3DDevice9 *d,IDirect3DSurface9 *src,const RECT *sr,IDirect3DSurface9 *dst,const RECT *dr,D3DTEXTUREFILTERTYPE filter)
{struct pw_d3d9_texture_request q={0};q.operation=PW_D3D9_TEXTURE_STRETCH;rect(&q,sr);if(dr){q.has_destination_rect=1;q.destination_left=dr->left;q.destination_top=dr->top;q.destination_right=dr->right;q.destination_bottom=dr->bottom;}q.filter=filter;return copy_resources(d,(IUnknown *)src,(IUnknown *)dst,PW_D3D9_KIND_SURFACE,&q);}
static HRESULT WINAPI color_fill(IDirect3DDevice9 *d,IDirect3DSurface9 *surface,const RECT *r,D3DCOLOR color)
{
 struct proxy *found=NULL;struct pw_d3d9_texture_request q={0};struct pw_d3d9_texture_reply reply={0};HRESULT hr;
 AcquireSRWLockExclusive(&cache_lock);
 for(struct proxy *p=cache;p;p=p->next)if(&p->iface==(IUnknown *)surface && p->parent==d && p->kind==PW_D3D9_KIND_SURFACE){found=p;p->refs++;break;}
 ReleaseSRWLockExclusive(&cache_lock);if(!found)return D3DERR_INVALIDCALL;
 q.operation=PW_D3D9_TEXTURE_COLOR_FILL;q.color=color;rect(&q,r);hr=invoke(found,&q,&reply);release(found);return hr;
}
static HRESULT WINAPI rt_data(IDirect3DDevice9 *d,IDirect3DSurface9 *src,IDirect3DSurface9 *dst)
{struct pw_d3d9_texture_request q={0};q.operation=PW_D3D9_TEXTURE_RT_DATA;return copy_resources(d,(IUnknown *)src,(IUnknown *)dst,PW_D3D9_KIND_SURFACE,&q);}
void pw_d3d9_texture_proxy_install(IDirect3DDevice9Vtbl *table,const struct pw_d3d9_texture_proxy_ops *callbacks)
{ops=*callbacks;table->CreateTexture=create_texture;table->CreateOffscreenPlainSurface=create_surface;table->CreateRenderTarget=create_rt;table->CreateDepthStencilSurface=create_depth;table->UpdateTexture=update_texture;table->UpdateSurface=update_surface;table->StretchRect=stretch;table->ColorFill=color_fill;table->GetRenderTargetData=rt_data;}
