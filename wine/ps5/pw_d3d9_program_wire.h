/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_PROGRAM_WIRE_H
#define PW_D3D9_PROGRAM_WIRE_H
#include <stddef.h>
#include <stdint.h>
#define PW_D3D9_PROGRAM_CHUNK 4096u
#define PW_D3D9_PROGRAM_LIMIT (256u * 1024u)
#define PW_D3D9_PROGRAM_WIRE_MAX (32u + PW_D3D9_PROGRAM_CHUNK)
enum pw_d3d9_program_kind { PW_D3D9_PROGRAM_DECL=7, PW_D3D9_PROGRAM_VS, PW_D3D9_PROGRAM_PS };
enum pw_d3d9_program_op { PW_D3D9_PROGRAM_BEGIN=1, PW_D3D9_PROGRAM_WRITE, PW_D3D9_PROGRAM_COMMIT, PW_D3D9_PROGRAM_ABORT };
enum pw_d3d9_program_result { PW_D3D9_PROGRAM_OK, PW_D3D9_PROGRAM_INVALID, PW_D3D9_PROGRAM_SMALL, PW_D3D9_PROGRAM_STALE, PW_D3D9_PROGRAM_BUSY, PW_D3D9_PROGRAM_EXHAUSTED };
/* Local DTO. Data bytes are canonical little endian DWORD shader tokens or
 * explicit WORD stream/offset and BYTE type/method/usage/index declaration fields. */
struct pw_d3d9_program_request {
 uint32_t operation,kind,total,offset,count;
 uint64_t transfer;
 unsigned char data[PW_D3D9_PROGRAM_CHUNK];
};
struct pw_d3d9_program_reply {
 uint32_t operation,hresult,id,generation;
 uint64_t transfer;
};
/* Reader supplies one native-valued token, with bounds/fault handling owned by
 * the caller. It returns zero on success. Measurement stops exactly at END. */
typedef int (*pw_d3d9_program_reader)(void *,size_t,uint32_t *);
int pw_d3d9_program_measure(uint32_t,pw_d3d9_program_reader,void *,size_t,size_t *);
int pw_d3d9_program_validate(uint32_t,const void *,size_t);
int pw_d3d9_program_encode(void *,size_t,size_t *,const struct pw_d3d9_program_request *);
int pw_d3d9_program_decode(struct pw_d3d9_program_request *,const void *,size_t);
int pw_d3d9_program_reply_encode(void *,size_t,size_t *,const struct pw_d3d9_program_reply *);
int pw_d3d9_program_reply_decode(struct pw_d3d9_program_reply *,const void *,size_t);
/* Caller owns storage and serializes access. COMMIT validates but retains the
 * bytes until finish; backend creation happens only after successful COMMIT.
 * A finished/aborted transfer cannot be reused, including after failed create. */
struct pw_d3d9_program_upload {
 unsigned char *storage;
 size_t capacity;
 uint64_t next,transfer;
 uint32_t kind,total,received,ready;
};
void pw_d3d9_program_upload_init(struct pw_d3d9_program_upload *,void *,size_t);
int pw_d3d9_program_upload_apply(struct pw_d3d9_program_upload *,const struct pw_d3d9_program_request *,uint64_t *);
void pw_d3d9_program_upload_finish(struct pw_d3d9_program_upload *);
#endif
