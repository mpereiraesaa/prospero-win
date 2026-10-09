/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_BUFFER_CLIENT_H
#define PW_D3D9_BUFFER_CLIENT_H
#include "../pw_d3d9_resource_wire.h"
/* Local PE32 state, never serialized. Caller serializes each resource and keeps
 * its proxy/service object alive while this state exists. A synchronous call
 * returns the actual reply HRESULT. fail cancels the session on corruption or
 * failed cleanup so a retained backend mapping cannot outlive its service. */
struct pw_d3d9_buffer_client {
 void *context;
 uint32_t (*call)(void *,struct pw_d3d9_object_ref,const struct pw_d3d9_resource_request *,struct pw_d3d9_resource_reply *);
 void (*fail)(void *,uint32_t);
 struct pw_d3d9_object_ref object;
 void *data;
 uint64_t generation;
 uint32_t length,flags,last_cleanup_result;
};
uint32_t pw_d3d9_buffer_client_lock(struct pw_d3d9_buffer_client *,uint32_t offset,uint32_t length,uint32_t flags,void **);
uint32_t pw_d3d9_buffer_client_unlock(struct pw_d3d9_buffer_client *);
uint32_t pw_d3d9_buffer_client_cancel(struct pw_d3d9_buffer_client *);
uint32_t pw_d3d9_buffer_client_staging_bytes(void);
#endif
