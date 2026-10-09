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
