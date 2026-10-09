/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_GAMMA_WIRE_H
#define PW_D3D9_GAMMA_WIRE_H
#include <stdint.h>
#include <stddef.h>
#define PW_D3D9_GAMMA_WORDS 768u
#define PW_D3D9_GAMMA_REQUEST_BYTES 1560u
#define PW_D3D9_GAMMA_REPLY_BYTES 1552u
/* Outer opcode31. Ordered red, green, blue, 256 WORDs each. */
enum pw_d3d9_gamma_method {PW_D3D9_GAMMA_SET=21,PW_D3D9_GAMMA_GET=22};
struct pw_d3d9_gamma_request {uint32_t method,swapchain,flags,has_ramp;uint16_t ramp[PW_D3D9_GAMMA_WORDS];};
/* hresult is transport/dispatch status, never an invented native return value. */
struct pw_d3d9_gamma_reply {uint32_t method,hresult,has_ramp;uint16_t ramp[PW_D3D9_GAMMA_WORDS];};
int pw_d3d9_gamma_request_encode(void *,size_t,const struct pw_d3d9_gamma_request *);
int pw_d3d9_gamma_request_decode(struct pw_d3d9_gamma_request *,const void *,size_t);
int pw_d3d9_gamma_reply_encode(void *,size_t,const struct pw_d3d9_gamma_request *,const struct pw_d3d9_gamma_reply *);
int pw_d3d9_gamma_reply_decode(struct pw_d3d9_gamma_reply *,const struct pw_d3d9_gamma_request *,const void *,size_t);
#endif
