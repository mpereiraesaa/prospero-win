/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_RESOURCE_WIRE_H
#define PW_D3D9_RESOURCE_WIRE_H
#include <stddef.h>
#include <stdint.h>
#include "pw_d3d9_objects.h"
#define PW_D3D9_RESOURCE_VERSION 1u
#define PW_D3D9_RESOURCE_CHUNK 4096u
#define PW_D3D9_RESOURCE_MAX_WIRE (32u + PW_D3D9_RESOURCE_CHUNK)
#define PW_D3D9_RESOURCE_MAX_LOCK (64u * 1024u * 1024u)
enum pw_d3d9_resource_operation {
 PW_D3D9_RESOURCE_CREATE_VB=1, PW_D3D9_RESOURCE_CREATE_IB,
 PW_D3D9_RESOURCE_DESC, PW_D3D9_RESOURCE_LOCK, PW_D3D9_RESOURCE_READ,
 PW_D3D9_RESOURCE_WRITE, PW_D3D9_RESOURCE_UNLOCK, PW_D3D9_RESOURCE_CANCEL_LOCK
};
enum pw_d3d9_resource_result {
 PW_D3D9_RESOURCE_OK, PW_D3D9_RESOURCE_INVALID, PW_D3D9_RESOURCE_SMALL,
 PW_D3D9_RESOURCE_UNSUPPORTED
};
/* Local DTOs only. Target object/device identity is in the outer frame.
 * format_fvf is FVF for VB creation and format for IB creation. */
struct pw_d3d9_resource_request {
 uint32_t operation, length, usage, format_fvf, pool, offset, flags, count;
 uint64_t lock_generation;
 unsigned char data[PW_D3D9_RESOURCE_CHUNK];
};
struct pw_d3d9_buffer_desc { uint32_t format, type, usage, pool, size, fvf; };
struct pw_d3d9_resource_reply {
 uint32_t operation, hresult;
 struct pw_d3d9_object_ref object;
 struct pw_d3d9_buffer_desc desc;
 uint64_t lock_generation;
 uint32_t length, offset, count;
 unsigned char data[PW_D3D9_RESOURCE_CHUNK];
};
/* Canonical little-endian payloads, exact lengths and zero reserved words.
 * Outputs are committed only on success; storage must not overlap input.
 * Failed HRESULT replies carry no output fields. Shared handles unsupported. */
int pw_d3d9_resource_request_encode(void *,size_t,size_t *,const struct pw_d3d9_resource_request *);
int pw_d3d9_resource_request_decode(struct pw_d3d9_resource_request *,const void *,size_t);
int pw_d3d9_resource_reply_encode(void *,size_t,size_t *,const struct pw_d3d9_resource_reply *);
int pw_d3d9_resource_reply_decode(struct pw_d3d9_resource_reply *,const void *,size_t);
#endif
