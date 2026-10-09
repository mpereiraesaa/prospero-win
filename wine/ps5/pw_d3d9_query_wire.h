/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_QUERY_WIRE_H
#define PW_D3D9_QUERY_WIRE_H
#include <stdint.h>
#include <stddef.h>
#include "pw_d3d9_objects.h"
#define PW_D3D9_QUERY_VERSION 1u
#define PW_D3D9_QUERY_DATA_MAX 16u
#define PW_D3D9_QUERY_WIRE_MAX 48u
/* Outer opcode29; kind11. No interface or output pointer crosses the wire. */
enum pw_d3d9_query_method {PW_D3D9_QUERY_CREATE=118,PW_D3D9_QUERY_ISSUE=6,PW_D3D9_QUERY_DATA=7};
struct pw_d3d9_query_request {uint32_t method,type,flags,size,has_data,want_object;uint8_t data[PW_D3D9_QUERY_DATA_MAX];};
struct pw_d3d9_query_reply {uint32_t method,hresult,type,size,count;struct pw_d3d9_object_ref object;uint8_t data[PW_D3D9_QUERY_DATA_MAX];};
/* ABI-stable POD query family. Zero means unsupported. */
uint32_t pw_d3d9_query_type_size(uint32_t);
int pw_d3d9_query_request_encode(void *,size_t,size_t *,const struct pw_d3d9_query_request *);
int pw_d3d9_query_request_decode(struct pw_d3d9_query_request *,const void *,size_t);
int pw_d3d9_query_reply_encode(void *,size_t,size_t *,const struct pw_d3d9_query_request *,const struct pw_d3d9_query_reply *);
int pw_d3d9_query_reply_decode(struct pw_d3d9_query_reply *,const struct pw_d3d9_query_request *,const void *,size_t);
#endif
