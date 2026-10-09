/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_PROGRAM_QUERY_H
#define PW_D3D9_PROGRAM_QUERY_H
#include "pw_d3d9_program_wire.h"
enum pw_d3d9_program_query_op { PW_D3D9_PROGRAM_SIZE=1, PW_D3D9_PROGRAM_READ };
/* capacity and size are native API units: declaration elements, shader bytes.
 * offset, count and total are canonical BYTES. No addresses cross the wire. */
struct pw_d3d9_program_query_request { uint32_t operation,kind,capacity,offset,count; };
struct pw_d3d9_program_query_reply {
 uint32_t operation,kind,hresult,size,total,offset,count;
 unsigned char data[PW_D3D9_PROGRAM_CHUNK];
};
int pw_d3d9_program_query_encode(void *,size_t,size_t *,const struct pw_d3d9_program_query_request *);
int pw_d3d9_program_query_decode(struct pw_d3d9_program_query_request *,const void *,size_t);
int pw_d3d9_program_query_reply_encode(void *,size_t,size_t *,const struct pw_d3d9_program_query_request *,const struct pw_d3d9_program_query_reply *);
int pw_d3d9_program_query_reply_decode(struct pw_d3d9_program_query_reply *,const struct pw_d3d9_program_query_request *,const void *,size_t);
#endif
