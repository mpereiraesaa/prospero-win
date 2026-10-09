/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_CURSOR_WIRE_H
#define PW_D3D9_CURSOR_WIRE_H
#include <stddef.h>
#include <stdint.h>
/* 10: hotspot x/y and typed surface; 11: signed x/y bits and flags;
 * 12: arg0 BOOL bits. No native pointers or HWNDs. */
struct pw_d3d9_cursor_request {uint32_t method,arg0,arg1,flags,id,generation;};
struct pw_d3d9_cursor_reply {uint32_t method,hresult,value;};
int pw_d3d9_cursor_encode(void *,size_t,size_t *,const struct pw_d3d9_cursor_request *);
int pw_d3d9_cursor_decode(struct pw_d3d9_cursor_request *,const void *,size_t);
int pw_d3d9_cursor_reply_encode(void *,size_t,size_t *,const struct pw_d3d9_cursor_reply *);
int pw_d3d9_cursor_reply_decode(struct pw_d3d9_cursor_reply *,const void *,size_t);
#endif
