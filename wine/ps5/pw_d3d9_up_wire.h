/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_UP_WIRE_H
#define PW_D3D9_UP_WIRE_H
#include <stddef.h>
#include <stdint.h>
#define PW_D3D9_UP_CHUNK 4096u
#define PW_D3D9_UP_LIMIT (64u * 1024u * 1024u)
#define PW_D3D9_UP_WIRE_MAX (64u + PW_D3D9_UP_CHUNK)
enum pw_d3d9_up_op { PW_D3D9_UP_BEGIN=1, PW_D3D9_UP_WRITE, PW_D3D9_UP_COMMIT, PW_D3D9_UP_ABORT };
enum pw_d3d9_up_result { PW_D3D9_UP_OK, PW_D3D9_UP_INVALID, PW_D3D9_UP_SMALL, PW_D3D9_UP_STALE, PW_D3D9_UP_BUSY, PW_D3D9_UP_EXHAUSTED };
struct pw_d3d9_up_draw {
 uint32_t method,primitive_type,min_vertex,num_vertices,primitive_count,stride,index_format;
 uint32_t vertex_bytes,index_bytes;
};
struct pw_d3d9_up_request {
 uint32_t operation;
 struct pw_d3d9_up_draw draw;
 uint32_t offset,count;
 uint64_t transfer;
 unsigned char data[PW_D3D9_UP_CHUNK];
};
struct pw_d3d9_up_reply { uint32_t operation,hresult; uint64_t transfer; };
/* Computes the exact DXVK 2.6.2 caller-read span, including indexed prefix.
 * Does not read caller data; output is unchanged on overflow/invalid input. */
int pw_d3d9_up_measure(struct pw_d3d9_up_draw *);
int pw_d3d9_up_encode(void *,size_t,size_t *,const struct pw_d3d9_up_request *);
int pw_d3d9_up_decode(struct pw_d3d9_up_request *,const void *,size_t);
int pw_d3d9_up_reply_encode(void *,size_t,size_t *,const struct pw_d3d9_up_reply *);
int pw_d3d9_up_reply_decode(struct pw_d3d9_up_reply *,const void *,size_t);
/* One serialized upload per device. Caller owns bounded storage. COMMIT
 * freezes bytes until native dispatch completes and finish consumes the ID. */
struct pw_d3d9_up_upload {
 unsigned char *storage;size_t capacity;
 uint64_t next,transfer;
 struct pw_d3d9_up_draw draw;
 uint32_t total,received,ready;
};
void pw_d3d9_up_upload_init(struct pw_d3d9_up_upload *,void *,size_t);
int pw_d3d9_up_upload_apply(struct pw_d3d9_up_upload *,const struct pw_d3d9_up_request *,uint64_t *);
void pw_d3d9_up_upload_finish(struct pw_d3d9_up_upload *);
#endif
