/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_COMMAND_POLICY_H
#define PW_D3D9_COMMAND_POLICY_H
#include "pw_d3d9_command_wire.h"
/* Expanded constants/Transform admission requires this additional HELLO bit.
 * Both peers must advertise it; first-wave batch admission alone is insufficient. */
#define PW_D3D9_COMMAND_POLICY_VERSION 2u
#define PW_D3D9_COMMAND_POLICY_FEATURE 16384u
/* Pinned DXVK5fde742b immediate-S_OK subset, for already owned command data.
 * Zero means synchronous fallback, not an API error. Does not authorize queue
 * admission: session health, ordering, device pins and negotiated policy still
 * require validation. No pointers are read and no command is modified. */
/* Exact pinned-backend constant validation order. Returns1 for a normalized
 * constant call,0 for another method,-1 for INVALIDCALL. Effective count is
 * unchanged on failure. Creation flags are immutable; current SWVP state does
 * not alter the layout. NULL data is legal when the clamped count is zero. */
static inline int pw_d3d9_command_constant_count(uint32_t method,uint32_t start,
        uint32_t count,uint32_t creation_flags,int has_data,uint32_t *effective)
{
    uint32_t software,hardware;
    int vertex=method==94 || method==96 || method==98;
    int floating=method==94 || method==109;
    if(!vertex && method!=109 && method!=111 && method!=113)return 0;
    software=vertex?(floating?8192u:2048u):(floating?224u:16u);
    hardware=vertex && !(creation_flags&0xa0u)?(floating?256u:16u):software;
    if(!effective || count>UINT32_MAX-start || start+count>software)return -1;
    uint32_t n=start>=hardware?0:count<hardware-start?count:hardware-start;
    if(n && !has_data)return -1;
    *effective=n;return 1;
}
int pw_d3d9_command_can_queue(const struct pw_d3d9_command *);
#endif
