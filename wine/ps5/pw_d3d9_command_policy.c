/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_command_policy.h"

int pw_d3d9_command_can_queue(const struct pw_d3d9_command *c)
{
    const struct pw_d3d9_command_schema *s;
    size_t bytes;
    unsigned i;
    if (!c || !(s = pw_d3d9_command_schema(c->method)) || s->object_words ||
        pw_d3d9_command_data_bytes(c->method, c->args, &bytes) || bytes != c->data_bytes)
        return 0;
    /* Canonical decoded commands have no hidden argument words. */
    for (i = s->words; i < PW_D3D9_COMMAND_WORDS; ++i)
        if (c->args[i]) return 0;
    switch (c->method) {
    case 44: /* SetTransform: all defined matrices fit the fixed backend array. */
        return c->args[0]==2 || c->args[0]==3 ||
               (c->args[0]>=16 && c->args[0]<=23) ||
               (c->args[0]>=256 && c->args[0]<=511);
    case 94:case 96:case 98:case 109:case 111:case 113: {
        uint32_t count;
        /* Fully owned data is non-NULL. Validate against the software range;
         * native layout clamping can only reduce the owned count. */
        if(pw_d3d9_command_constant_count(c->method,c->args[0],c->args[1],0xa0u,1,&count)!=1)return 0;
        if(c->method==98 || c->method==113)
            for(i=0;i<c->args[1];i++)if(c->data.words[i]>1)return 0;
        return 1;
    }
    case 47: /* SetViewport: complete owned24-byte struct. */
    case 49: /* SetMaterial: complete owned68-byte struct. */
    case 75: /* SetScissorRect: complete owned16-byte struct. */
    case 57: /* SetRenderState: unknown indices are backend S_OK no-ops. */
        return 1;
    case 67: /* SetTextureStageState: preserve backend Stage clamp. */
        return (c->args[1] >= 1 && c->args[1] <= 11) ||
               (c->args[1] >= 22 && c->args[1] <= 24) ||
               (c->args[1] >= 26 && c->args[1] <= 28) || c->args[1] == 32;
    case 69: /* SetSamplerState: mapped PS, displacement, and VS slots. */
        return (c->args[0] < 16 || (c->args[0] >= 256 && c->args[0] <= 260)) &&
               c->args[1] >= 1 && c->args[1] <= 13;
    default:
        return 0;
    }
}
