/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_STATEBLOCK_WIRE_H
#define PW_D3D9_STATEBLOCK_WIRE_H
#include <stddef.h>
#include <stdint.h>
#include "pw_d3d9_objects.h"
#define PW_D3D9_STATEBLOCK_BYTES 16u
#define PW_D3D9_STATEBLOCK_VERSION 1u
/* Outer opcode 26 carries typed target ID/generation: device for 59/60/61,
 * stateblock for 4/5. GetDevice uses the proxy's strong local parent identity. */
enum pw_d3d9_stateblock_method {PW_D3D9_SB_CAPTURE=4,PW_D3D9_SB_APPLY=5,
 PW_D3D9_SB_CREATE=59,PW_D3D9_SB_BEGIN=60,PW_D3D9_SB_END=61};
enum pw_d3d9_stateblock_status {PW_D3D9_SB_OK=0,PW_D3D9_SB_INVALID=-1,PW_D3D9_SB_UNSUPPORTED=-2};
struct pw_d3d9_stateblock_request {uint32_t method,type;};
struct pw_d3d9_stateblock_reply {uint32_t hresult;struct pw_d3d9_object_ref object;};
int pw_d3d9_stateblock_validate(const struct pw_d3d9_stateblock_request *);
int pw_d3d9_stateblock_encode(void *,size_t,const struct pw_d3d9_stateblock_request *);
int pw_d3d9_stateblock_decode(struct pw_d3d9_stateblock_request *,const void *,size_t);
int pw_d3d9_stateblock_reply_encode(void *,size_t,const struct pw_d3d9_stateblock_request *,const struct pw_d3d9_stateblock_reply *);
int pw_d3d9_stateblock_reply_decode(struct pw_d3d9_stateblock_reply *,const struct pw_d3d9_stateblock_request *,const void *,size_t);
#endif
