/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_TEXTURE_CLIENT_H
#define PW_D3D9_TEXTURE_CLIENT_H
#include "../pw_d3d9_texture_wire.h"
struct pw_d3d9_texture_client {
 void *context;
 uint32_t (*call)(void *,struct pw_d3d9_object_ref,const struct pw_d3d9_texture_request *,struct pw_d3d9_texture_reply *);
 void (*fail)(void *,uint32_t);
 struct pw_d3d9_object_ref object;
 void *data;
 uint64_t generation;
 int32_t pitch;
 uint32_t length,rows,row_bytes,flags,last_cleanup_result;
};
/* Caller serializes this state and retains its proxy/service reference. Lock
 * accepts only a TEXTURE_LOCK request and returns owned low32 bits/pitch. */
uint32_t pw_d3d9_texture_client_lock(struct pw_d3d9_texture_client *,const struct pw_d3d9_texture_request *,int32_t *,void **);
uint32_t pw_d3d9_texture_client_unlock(struct pw_d3d9_texture_client *);
uint32_t pw_d3d9_texture_client_cancel(struct pw_d3d9_texture_client *);
#endif
