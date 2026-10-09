/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_native_texture.h"
#include "pw_d3d9_failure_diag.h"
#include <d3d9.h>
#include <string.h>
#define ACTIVE_LOCKS 128u
struct pw_d3d9_native_texture {
 union {IDirect3DTexture9 *texture;IDirect3DSurface9 *surface;IUnknown *unknown;} object;
 IDirect3DDevice9 *device;uint32_t kind,parked;
 IDirect3DSurface9 *locked_surface;
 unsigned char *mapping;int32_t pitch;
 uint64_t generation;uint32_t rows,row_bytes,length,written,readonly,slot;
};
static SRWLOCK locks_guard=SRWLOCK_INIT;
static struct {uintptr_t identity;struct pw_d3d9_native_texture *owner;} locks[ACTIVE_LOCKS];
static HRESULT reserve_lock(struct pw_d3d9_native_texture *r,IDirect3DSurface9 *surface)
{
 IUnknown *identity=NULL;HRESULT hr=IDirect3DSurface9_QueryInterface(surface,&IID_IUnknown,(void **)&identity);unsigned i,free_slot=ACTIVE_LOCKS;
 if(FAILED(hr))return hr;
 if(!identity)return E_FAIL;
 AcquireSRWLockExclusive(&locks_guard);
 for(i=0;i<ACTIVE_LOCKS;i++){
  if(locks[i].identity==(uintptr_t)identity){ReleaseSRWLockExclusive(&locks_guard);IUnknown_Release(identity);return E_NOTIMPL;}
  if(!locks[i].owner)free_slot=i;
 }
 if(free_slot!=ACTIVE_LOCKS){locks[free_slot].owner=r;locks[free_slot].identity=(uintptr_t)identity;r->slot=free_slot;}
 ReleaseSRWLockExclusive(&locks_guard);IUnknown_Release(identity);
 return free_slot==ACTIVE_LOCKS?E_OUTOFMEMORY:S_OK;
}
static void release_lock(struct pw_d3d9_native_texture *r)
{
 AcquireSRWLockExclusive(&locks_guard);locks[r->slot].identity=0;locks[r->slot].owner=NULL;ReleaseSRWLockExclusive(&locks_guard);
 IDirect3DSurface9_Release(r->locked_surface);r->locked_surface=NULL;r->mapping=NULL;r->length=r->rows=r->row_bytes=r->written=r->readonly=0;r->pitch=0;
}
static HRESULT unlock(struct pw_d3d9_native_texture *r)
{HRESULT hr=IDirect3DSurface9_UnlockRect(r->locked_surface);release_lock(r);return hr;}
static void describe(struct pw_d3d9_surface_desc *out,const D3DSURFACE_DESC *d)
{*out=(struct pw_d3d9_surface_desc){d->Format,d->Type,d->Usage,d->Pool,d->MultiSampleType,d->MultiSampleQuality,d->Width,d->Height};}
static HRESULT desc(struct pw_d3d9_native_texture *r,uint32_t level,D3DSURFACE_DESC *out)
{
 if(r->kind==PW_D3D9_KIND_TEXTURE_2D)return IDirect3DTexture9_GetLevelDesc(r->object.texture,level,out);
 if(level)return D3DERR_INVALIDCALL;
 return IDirect3DSurface9_GetDesc(r->object.surface,out);
}
static HRESULT select_surface(struct pw_d3d9_native_texture *r,uint32_t level,IDirect3DSurface9 **out)
{
 *out=NULL;
 if(r->kind==PW_D3D9_KIND_TEXTURE_2D)return IDirect3DTexture9_GetSurfaceLevel(r->object.texture,level,out);
 if(level)return D3DERR_INVALIDCALL;
 *out=r->object.surface;IDirect3DSurface9_AddRef(*out);return S_OK;
}
static int format_block(D3DFORMAT format,uint32_t *block,uint32_t *bytes)
{
 *block=1;
 switch(format){
 case D3DFMT_A8R8G8B8:case D3DFMT_X8R8G8B8:case D3DFMT_A8B8G8R8:case D3DFMT_X8B8G8R8:case D3DFMT_A2R10G10B10:case D3DFMT_A2B10G10R10:case D3DFMT_G16R16:*bytes=4;return 1;
 case D3DFMT_R8G8B8:*bytes=3;return 1;
 case D3DFMT_R5G6B5:case D3DFMT_X1R5G5B5:case D3DFMT_A1R5G5B5:case D3DFMT_A4R4G4B4:case D3DFMT_X4R4G4B4:case D3DFMT_A8L8:case D3DFMT_A8P8:case D3DFMT_L16:*bytes=2;return 1;
 case D3DFMT_A8:case D3DFMT_L8:case D3DFMT_P8:case D3DFMT_A4L4:*bytes=1;return 1;
 case D3DFMT_DXT1:*block=4;*bytes=8;return 1;
 case D3DFMT_DXT2:case D3DFMT_DXT3:case D3DFMT_DXT4:case D3DFMT_DXT5:*block=4;*bytes=16;return 1;
 default:return 0;
 }
}
uint32_t pw_d3d9_native_texture_destroy(struct pw_d3d9_native_texture *r)
{
 HRESULT hr=S_OK;if(!r)return hr;
 if(r->locked_surface)hr=unlock(r);
 if(r->object.unknown&&!r->parked)IUnknown_Release(r->object.unknown);
 if(r->device)IDirect3DDevice9_Release(r->device);
 HeapFree(GetProcessHeap(),0,r);return hr;
}
uintptr_t pw_d3d9_native_texture_identity(struct pw_d3d9_native_texture *r)
{
 IUnknown *identity=NULL;uintptr_t value;
 if(!r||r->parked||!r->object.unknown||FAILED(IUnknown_QueryInterface(r->object.unknown,&IID_IUnknown,(void **)&identity))||!identity)return 0;
 value=(uintptr_t)identity;IUnknown_Release(identity);return value;
}
void *pw_d3d9_native_texture_backend(struct pw_d3d9_native_texture *r)
{return r&&!r->parked?r->object.unknown:NULL;}
uint32_t pw_d3d9_native_texture_kind(struct pw_d3d9_native_texture *r)
{return r?r->kind:0;}
uint32_t pw_d3d9_native_texture_adopt(void *device,uint32_t kind,void *owned,struct pw_d3d9_native_texture **out)
{
 struct pw_d3d9_native_texture *r;HRESULT hr;IUnknown *object=owned;
 if(out)*out=NULL;
 if(!out||!device||!object){if(object)IUnknown_Release(object);return E_INVALIDARG;}
 if(kind!=PW_D3D9_KIND_TEXTURE_2D&&kind!=PW_D3D9_KIND_SURFACE){IUnknown_Release(object);return E_NOTIMPL;}
 r=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*r));if(!r){IUnknown_Release(object);return E_OUTOFMEMORY;}
 r->kind=kind;
 hr=IUnknown_QueryInterface(object,kind==PW_D3D9_KIND_TEXTURE_2D?&IID_IDirect3DTexture9:&IID_IDirect3DSurface9,(void **)&r->object.unknown);
 IUnknown_Release(object);
 if(SUCCEEDED(hr)&&!r->object.unknown)hr=E_FAIL;
 if(SUCCEEDED(hr)){
  hr=kind==PW_D3D9_KIND_TEXTURE_2D?IDirect3DTexture9_GetDevice(r->object.texture,&r->device):IDirect3DSurface9_GetDevice(r->object.surface,&r->device);
  if(SUCCEEDED(hr)&&r->device!=device)hr=D3DERR_INVALIDCALL;
 }
 if(FAILED(hr))pw_d3d9_native_texture_destroy(r);else *out=r;
 return (uint32_t)hr;
}
void pw_d3d9_native_texture_create(void *device,const struct pw_d3d9_texture_request *q,struct pw_d3d9_texture_reply *reply,struct pw_d3d9_native_texture **out)
{
 struct pw_d3d9_native_texture *r;D3DSURFACE_DESC d;HRESULT hr;
 memset(reply,0,sizeof(*reply));reply->operation=q->operation;reply->hresult=D3DERR_INVALIDCALL;*out=NULL;
 if(!device)return;
 if(q->operation!=PW_D3D9_TEXTURE_CREATE&&q->operation!=PW_D3D9_TEXTURE_CREATE_SURFACE&&q->operation!=PW_D3D9_TEXTURE_CREATE_RT&&q->operation!=PW_D3D9_TEXTURE_CREATE_DEPTH){reply->hresult=E_NOTIMPL;return;}
 r=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*r));if(!r){reply->hresult=E_OUTOFMEMORY;return;}
 r->kind=q->operation==PW_D3D9_TEXTURE_CREATE?PW_D3D9_KIND_TEXTURE_2D:PW_D3D9_KIND_SURFACE;
 if(r->kind==PW_D3D9_KIND_TEXTURE_2D)hr=IDirect3DDevice9_CreateTexture((IDirect3DDevice9 *)device,q->width,q->height,q->levels,q->usage,q->format,q->pool,&r->object.texture,NULL);
 else if(q->operation==PW_D3D9_TEXTURE_CREATE_RT)hr=IDirect3DDevice9_CreateRenderTarget((IDirect3DDevice9 *)device,q->width,q->height,q->format,q->multisample_type,q->multisample_quality,(BOOL)q->lockable,&r->object.surface,NULL);
 else if(q->operation==PW_D3D9_TEXTURE_CREATE_DEPTH)hr=IDirect3DDevice9_CreateDepthStencilSurface((IDirect3DDevice9 *)device,q->width,q->height,q->format,q->multisample_type,q->multisample_quality,(BOOL)q->discard,&r->object.surface,NULL);
 else hr=IDirect3DDevice9_CreateOffscreenPlainSurface((IDirect3DDevice9 *)device,q->width,q->height,q->format,q->pool,&r->object.surface,NULL);
 if(SUCCEEDED(hr)&&!r->object.unknown)hr=E_FAIL;
 if(SUCCEEDED(hr)){
  r->device=device;IDirect3DDevice9_AddRef(r->device);hr=desc(r,0,&d);
  if(SUCCEEDED(hr)){describe(&reply->desc,&d);reply->levels=r->kind==PW_D3D9_KIND_TEXTURE_2D?IDirect3DTexture9_GetLevelCount(r->object.texture):1;if(!reply->levels)hr=E_FAIL;}
 }
 if(FAILED(hr)){pw_d3d9_native_texture_destroy(r);memset(&reply->desc,0,sizeof(reply->desc));reply->levels=0;}else *out=r;
 pw_d3d9_texture_failure(stderr,q,(uint32_t)hr);
 reply->hresult=(uint32_t)hr;
}
static HRESULT begin(struct pw_d3d9_native_texture *r,const struct pw_d3d9_texture_request *q,struct pw_d3d9_texture_reply *reply)
{
 D3DSURFACE_DESC d;D3DLOCKED_RECT mapped={0};RECT rect;IDirect3DSurface9 *surface=NULL;
 HRESULT hr;uint32_t block,bytes;uint64_t row_bytes,rows,stride,length;
 if(r->locked_surface)return E_NOTIMPL;
 if(r->generation==UINT64_MAX)return E_FAIL;
 hr=desc(r,q->level,&d);if(FAILED(hr))return hr;
 if(!format_block(d.Format,&block,&bytes)||d.Width>INT32_MAX||d.Height>INT32_MAX)return E_NOTIMPL;
 rect=q->has_rect?(RECT){q->left,q->top,q->right,q->bottom}:(RECT){0,0,(LONG)d.Width,(LONG)d.Height};
 if(rect.left<0||rect.top<0||rect.right<=rect.left||rect.bottom<=rect.top||(uint32_t)rect.right>d.Width||(uint32_t)rect.bottom>d.Height)return D3DERR_INVALIDCALL;
 if((rect.left%block)||(rect.top%block)||((rect.right%block)&&(uint32_t)rect.right!=d.Width)||((rect.bottom%block)&&(uint32_t)rect.bottom!=d.Height))return E_NOTIMPL;
 row_bytes=((uint64_t)(rect.right-rect.left)+block-1)/block*bytes;rows=((uint64_t)(rect.bottom-rect.top)+block-1)/block;
 if(row_bytes>PW_D3D9_RESOURCE_MAX_LOCK||rows>PW_D3D9_RESOURCE_MAX_LOCK/row_bytes)return E_NOTIMPL;
 hr=select_surface(r,q->level,&surface);if(FAILED(hr))return hr;if(!surface)return E_FAIL;
 hr=reserve_lock(r,surface);if(FAILED(hr)){IDirect3DSurface9_Release(surface);return hr;}
 r->locked_surface=surface;
 hr=IDirect3DSurface9_LockRect(surface,&mapped,q->has_rect?&rect:NULL,q->flags);
 if(FAILED(hr)){release_lock(r);return hr;}
 stride=mapped.Pitch<0?-(int64_t)mapped.Pitch:mapped.Pitch;length=stride*(rows-1)+row_bytes;
 if(!mapped.pBits||length>UINT32_MAX||!pw_d3d9_texture_layout_valid(mapped.Pitch,(uint32_t)rows,(uint32_t)row_bytes,(uint32_t)length)){
  unlock(r);return E_NOTIMPL;
 }
 r->mapping=mapped.pBits;r->pitch=mapped.Pitch;r->rows=(uint32_t)rows;r->row_bytes=(uint32_t)row_bytes;r->length=(uint32_t)length;
 r->generation++;r->written=0;r->readonly=!!(q->flags&D3DLOCK_READONLY);
 reply->lock_generation=r->generation;reply->pitch=r->pitch;reply->rows=r->rows;reply->row_bytes=r->row_bytes;reply->length=r->length;return hr;
}
static void transfer(struct pw_d3d9_native_texture *r,uint32_t offset,unsigned char *data,uint32_t count,int write)
{
 uint32_t stride=r->pitch<0?(uint32_t)-(int64_t)r->pitch:(uint32_t)r->pitch;
 if(!write)memset(data,0,count);
 while(count){
  uint32_t row=offset/stride,column=offset%stride,n=stride-column,copy;
  if(n>count)n=count;
  copy=column<r->row_bytes?r->row_bytes-column:0;if(copy>n)copy=n;
  if(copy){
   uint32_t native_row=r->pitch<0?r->rows-1-row:row;
   unsigned char *native=r->mapping+(int64_t)native_row*r->pitch+column;
   if(write)memcpy(native,data,copy);else memcpy(data,native,copy);
  }
  offset+=n;data+=n;count-=n;
 }
}
void pw_d3d9_native_texture_call(struct pw_d3d9_native_texture *r,const struct pw_d3d9_texture_request *q,struct pw_d3d9_texture_reply *reply,struct pw_d3d9_native_texture **out)
{
 HRESULT hr=D3DERR_INVALIDCALL;D3DSURFACE_DESC d;struct pw_d3d9_native_texture *child;
 memset(reply,0,sizeof(*reply));reply->operation=q->operation;reply->hresult=hr;*out=NULL;if(!r||r->parked||!r->object.unknown)return;
 switch(q->operation){
 case PW_D3D9_TEXTURE_GET_PRIORITY:
  reply->value=r->kind==PW_D3D9_KIND_TEXTURE_2D?IDirect3DTexture9_GetPriority(r->object.texture):IDirect3DSurface9_GetPriority(r->object.surface);hr=S_OK;break;
 case PW_D3D9_TEXTURE_SET_PRIORITY:
  reply->value=r->kind==PW_D3D9_KIND_TEXTURE_2D?IDirect3DTexture9_SetPriority(r->object.texture,q->value):IDirect3DSurface9_SetPriority(r->object.surface,q->value);hr=S_OK;break;
 case PW_D3D9_TEXTURE_PRELOAD:
  if(r->kind==PW_D3D9_KIND_TEXTURE_2D)IDirect3DTexture9_PreLoad(r->object.texture);else IDirect3DSurface9_PreLoad(r->object.surface);
  hr=S_OK;break;
 case PW_D3D9_TEXTURE_GET_LOD:
  if(r->kind==PW_D3D9_KIND_TEXTURE_2D){reply->value=IDirect3DTexture9_GetLOD(r->object.texture);hr=S_OK;}break;
 case PW_D3D9_TEXTURE_SET_LOD:
  if(r->kind==PW_D3D9_KIND_TEXTURE_2D){reply->value=IDirect3DTexture9_SetLOD(r->object.texture,q->value);hr=S_OK;}break;
 case PW_D3D9_TEXTURE_GET_AUTOGEN_FILTER:
  if(r->kind==PW_D3D9_KIND_TEXTURE_2D){reply->value=IDirect3DTexture9_GetAutoGenFilterType(r->object.texture);hr=S_OK;}break;
 case PW_D3D9_TEXTURE_SET_AUTOGEN_FILTER:
  if(r->kind==PW_D3D9_KIND_TEXTURE_2D)hr=IDirect3DTexture9_SetAutoGenFilterType(r->object.texture,q->value);
  break;
 case PW_D3D9_TEXTURE_GENERATE_MIPS:
  if(r->kind==PW_D3D9_KIND_TEXTURE_2D){IDirect3DTexture9_GenerateMipSubLevels(r->object.texture);hr=S_OK;}break;
 case PW_D3D9_TEXTURE_DESC:hr=desc(r,q->level,&d);if(SUCCEEDED(hr)){describe(&reply->desc,&d);reply->levels=r->kind==PW_D3D9_KIND_TEXTURE_2D?IDirect3DTexture9_GetLevelCount(r->object.texture):1;if(!reply->levels)hr=E_FAIL;}break;
 case PW_D3D9_TEXTURE_SURFACE_LEVEL:
  if(r->kind!=PW_D3D9_KIND_TEXTURE_2D)break;
  child=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*child));if(!child){hr=E_OUTOFMEMORY;break;}
  child->kind=PW_D3D9_KIND_SURFACE;hr=select_surface(r,q->level,&child->object.surface);
  if(SUCCEEDED(hr)&&!child->object.surface)hr=E_FAIL;
  if(SUCCEEDED(hr)){
   child->device=r->device;IDirect3DDevice9_AddRef(child->device);hr=desc(child,0,&d);
   if(SUCCEEDED(hr)){describe(&reply->desc,&d);reply->levels=1;*out=child;}
  }
  if(FAILED(hr))pw_d3d9_native_texture_destroy(child);
  break;
 case PW_D3D9_TEXTURE_LOCK:hr=begin(r,q,reply);break;
 case PW_D3D9_TEXTURE_READ:case PW_D3D9_TEXTURE_WRITE:
  if(!r->mapping||q->lock_generation!=r->generation||!q->count||q->count>PW_D3D9_RESOURCE_CHUNK||q->offset>r->length||q->count>r->length-q->offset)break;
  if(q->operation==PW_D3D9_TEXTURE_WRITE){
   if(r->readonly||q->offset!=r->written)break;
   /* The decoded request owns these bytes; transfer never retains them. */
   transfer(r,q->offset,(unsigned char *)q->data,q->count,1);r->written+=q->count;
  }else{
   transfer(r,q->offset,reply->data,q->count,0);reply->count=q->count;reply->offset=q->offset;reply->lock_generation=r->generation;
  }hr=S_OK;break;
 case PW_D3D9_TEXTURE_UNLOCK:case PW_D3D9_TEXTURE_CANCEL_LOCK:
  if(!r->locked_surface||q->lock_generation!=r->generation)break;
  if(q->operation==PW_D3D9_TEXTURE_UNLOCK&&!r->readonly&&r->written!=r->length)break;
  hr=unlock(r);break;
 case PW_D3D9_TEXTURE_COLOR_FILL:
  if(r->kind==PW_D3D9_KIND_SURFACE){RECT rect={q->left,q->top,q->right,q->bottom};hr=IDirect3DDevice9_ColorFill(r->device,r->object.surface,q->has_rect?&rect:NULL,q->color);}break;
 case PW_D3D9_TEXTURE_DIRTY:
  if(r->kind==PW_D3D9_KIND_TEXTURE_2D){RECT rect={q->left,q->top,q->right,q->bottom};hr=IDirect3DTexture9_AddDirtyRect(r->object.texture,q->has_rect?&rect:NULL);}break;
 default:hr=E_NOTIMPL;break;
 }
 pw_d3d9_texture_failure(stderr,q,(uint32_t)hr);
 reply->hresult=(uint32_t)hr;
}
void pw_d3d9_native_texture_container(struct pw_d3d9_native_texture *r,const struct pw_d3d9_texture_request *q,
 struct pw_d3d9_texture_reply *reply,void **owned)
{
 static const IID *const interfaces[]={NULL,&IID_IUnknown,&IID_IDirect3DDevice9,&IID_IDirect3DResource9,
  &IID_IDirect3DBaseTexture9,&IID_IDirect3DTexture9,&IID_IDirect3DSwapChain9};
 IUnknown *raw=NULL;IDirect3DDevice9 *device=NULL,*actual=NULL;IDirect3DTexture9 *texture=NULL;HRESULT hr=D3DERR_INVALIDCALL,container_hr;
 memset(reply,0,sizeof(*reply));reply->operation=q->operation;if(owned)*owned=NULL;
 if(!owned||!r||r->parked||!r->object.unknown||r->kind!=PW_D3D9_KIND_SURFACE||q->operation!=PW_D3D9_TEXTURE_CONTAINER||q->value<1||q->value>PW_D3D9_CONTAINER_SWAPCHAIN)goto done;
 hr=IDirect3DSurface9_GetContainer(r->object.surface,interfaces[q->value],(void **)&raw);
 if(FAILED(hr))goto done;
 if(!raw){hr=E_FAIL;goto done;}
 container_hr=hr;
 hr=IUnknown_QueryInterface(raw,&IID_IDirect3DDevice9,(void **)&device);
 if(SUCCEEDED(hr)){
  if(!device){hr=E_FAIL;goto done;}
  if(device!=r->device){hr=D3DERR_INVALIDCALL;goto done;}
  reply->container_kind=PW_D3D9_KIND_DEVICE;*owned=device;device=NULL;hr=container_hr;goto done;
 }
 if(hr!=E_NOINTERFACE)goto done;
 hr=IUnknown_QueryInterface(raw,&IID_IDirect3DTexture9,(void **)&texture);
 if(FAILED(hr))goto done; /* includes unsupported swapchain/cube containers */
 if(!texture){hr=E_FAIL;goto done;}
 hr=IDirect3DTexture9_GetDevice(texture,&actual);if(FAILED(hr))goto done;
 if(!actual||actual!=r->device){hr=D3DERR_INVALIDCALL;goto done;}
 reply->levels=IDirect3DTexture9_GetLevelCount(texture);
 if(!reply->levels){hr=E_FAIL;goto done;}
 reply->container_kind=PW_D3D9_KIND_TEXTURE_2D;*owned=texture;texture=NULL;hr=container_hr;
 done:
 if(actual)IDirect3DDevice9_Release(actual);
 if(device)IDirect3DDevice9_Release(device);
 if(texture)IDirect3DTexture9_Release(texture);
 if(raw)IUnknown_Release(raw);
 if(FAILED(hr)){reply->levels=0;reply->container_kind=0;}
 pw_d3d9_texture_failure(stderr,q,(uint32_t)hr);
 reply->hresult=(uint32_t)hr;
}
void pw_d3d9_native_texture_copy(void *device,struct pw_d3d9_native_texture *source,struct pw_d3d9_native_texture *destination,const struct pw_d3d9_texture_request *q,struct pw_d3d9_texture_reply *reply)
{
 HRESULT hr=D3DERR_INVALIDCALL;memset(reply,0,sizeof(*reply));reply->operation=q->operation;reply->hresult=hr;
 if(!device||!source||!destination||source->parked||destination->parked||!source->object.unknown||!destination->object.unknown||source->device!=device||destination->device!=device)return;
 if(q->operation==PW_D3D9_TEXTURE_UPDATE&&source->kind==PW_D3D9_KIND_TEXTURE_2D&&destination->kind==PW_D3D9_KIND_TEXTURE_2D)
  hr=IDirect3DDevice9_UpdateTexture((IDirect3DDevice9 *)device,(IDirect3DBaseTexture9 *)source->object.texture,(IDirect3DBaseTexture9 *)destination->object.texture);
 else if(q->operation==PW_D3D9_TEXTURE_UPDATE_SURFACE&&source->kind==PW_D3D9_KIND_SURFACE&&destination->kind==PW_D3D9_KIND_SURFACE){
  RECT rect={q->left,q->top,q->right,q->bottom};POINT point={q->x,q->y};
  hr=IDirect3DDevice9_UpdateSurface((IDirect3DDevice9 *)device,source->object.surface,q->has_rect?&rect:NULL,destination->object.surface,q->has_point?&point:NULL);
 }
 else if(q->operation==PW_D3D9_TEXTURE_RT_DATA&&source->kind==PW_D3D9_KIND_SURFACE&&destination->kind==PW_D3D9_KIND_SURFACE)
  hr=IDirect3DDevice9_GetRenderTargetData((IDirect3DDevice9 *)device,source->object.surface,destination->object.surface);
 else if(q->operation==PW_D3D9_TEXTURE_STRETCH&&source->kind==PW_D3D9_KIND_SURFACE&&destination->kind==PW_D3D9_KIND_SURFACE){
  RECT source_rect={q->left,q->top,q->right,q->bottom};
  RECT destination_rect={q->destination_left,q->destination_top,q->destination_right,q->destination_bottom};
  hr=IDirect3DDevice9_StretchRect((IDirect3DDevice9 *)device,source->object.surface,q->has_rect?&source_rect:NULL,destination->object.surface,q->has_destination_rect?&destination_rect:NULL,q->filter);
 }
 pw_d3d9_texture_failure(stderr,q,(uint32_t)hr);
 reply->hresult=(uint32_t)hr;
}

int pw_d3d9_native_texture_can_park(struct pw_d3d9_native_texture *r)
{return r&&r->kind==PW_D3D9_KIND_SURFACE&&r->object.unknown&&!r->parked&&!r->locked_surface;}
uint32_t pw_d3d9_native_texture_park(struct pw_d3d9_native_texture *r)
{
 if(!pw_d3d9_native_texture_can_park(r))return D3DERR_INVALIDCALL;
 r->parked=1;IUnknown_Release(r->object.unknown);return S_OK;
}
void pw_d3d9_native_texture_unpark(struct pw_d3d9_native_texture *r,int restore)
{
 if(!r||!r->parked)return;
 if(restore)IUnknown_AddRef(r->object.unknown);else r->object.unknown=NULL;
 r->parked=0;
}
