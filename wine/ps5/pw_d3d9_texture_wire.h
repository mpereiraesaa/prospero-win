/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_TEXTURE_WIRE_H
#define PW_D3D9_TEXTURE_WIRE_H
#include "pw_d3d9_resource_wire.h"
#define PW_D3D9_TEXTURE_VERSION 2u
#define PW_D3D9_TEXTURE_MAX_WIRE PW_D3D9_RESOURCE_MAX_WIRE
enum pw_d3d9_texture_operation {
 PW_D3D9_TEXTURE_CREATE=1, PW_D3D9_TEXTURE_CREATE_SURFACE,
 PW_D3D9_TEXTURE_DESC, PW_D3D9_TEXTURE_SURFACE_LEVEL,
 PW_D3D9_TEXTURE_LOCK, PW_D3D9_TEXTURE_READ, PW_D3D9_TEXTURE_WRITE,
 PW_D3D9_TEXTURE_UNLOCK, PW_D3D9_TEXTURE_CANCEL_LOCK,
 PW_D3D9_TEXTURE_DIRTY, PW_D3D9_TEXTURE_UPDATE, PW_D3D9_TEXTURE_UPDATE_SURFACE,
 PW_D3D9_TEXTURE_CREATE_RT, PW_D3D9_TEXTURE_CREATE_DEPTH, PW_D3D9_TEXTURE_STRETCH
};
struct pw_d3d9_surface_desc {
 uint32_t format,type,usage,pool,multisample_type,multisample_quality,width,height;
};
struct pw_d3d9_texture_request {
 uint32_t operation,width,height,levels,usage,format,pool,level,flags,has_rect;
 int32_t left,top,right,bottom;
 uint32_t multisample_type,multisample_quality,lockable,discard,filter,has_destination_rect;
 int32_t destination_left,destination_top,destination_right,destination_bottom;
 uint32_t has_point;
 int32_t x,y;
 struct pw_d3d9_object_ref source,destination;
 uint64_t lock_generation;
 uint32_t offset,count;
 unsigned char data[PW_D3D9_RESOURCE_CHUNK];
};
struct pw_d3d9_texture_reply {
 uint32_t operation,hresult;
 struct pw_d3d9_object_ref object;
 uint32_t levels;
 struct pw_d3d9_surface_desc desc;
 uint64_t lock_generation;
 int32_t pitch;
 uint32_t rows,row_bytes,length,offset,count;
 unsigned char data[PW_D3D9_RESOURCE_CHUNK];
};
/* Local DTOs only. Signed pitch is preserved; padding in copied staging is zero
 * and never copied to/from the backend. length=abs(pitch)*(rows-1)+row_bytes.
 * The client adjusts its returned pointer for negative pitch. */
int pw_d3d9_texture_layout_valid(int32_t pitch,uint32_t rows,uint32_t row_bytes,uint32_t length);
int pw_d3d9_texture_request_encode(void *,size_t,size_t *,const struct pw_d3d9_texture_request *);
int pw_d3d9_texture_request_decode(struct pw_d3d9_texture_request *,const void *,size_t);
int pw_d3d9_texture_reply_encode(void *,size_t,size_t *,const struct pw_d3d9_texture_reply *);
int pw_d3d9_texture_reply_decode(struct pw_d3d9_texture_reply *,const void *,size_t);
#endif
