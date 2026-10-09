/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_draw_shadow.h"
#include <string.h>
void pw_d3d9_draw_init(struct pw_d3d9_draw_shadow *s)
{
    if (s) { memset(s, 0, sizeof(*s)); s->active = PW_D3D9_DECL_NULL; }
}
void pw_d3d9_draw_invalidate(struct pw_d3d9_draw_shadow *s)
{
    if (s) s->active = PW_D3D9_DECL_UNKNOWN;
}
void pw_d3d9_draw_declaration(struct pw_d3d9_draw_shadow *s, int nonnull, uint32_t hr)
{
    enum pw_d3d9_decl_state value = nonnull ? PW_D3D9_DECL_PRESENT : PW_D3D9_DECL_NULL;
    if (!s || hr) return;
    if (s->recording) { s->pending.captures_decl = 1; s->pending.declaration = value; }
    else s->active = value;
}
void pw_d3d9_draw_fvf(struct pw_d3d9_draw_shadow *s, uint32_t fvf, uint32_t hr)
{
    if (fvf) pw_d3d9_draw_declaration(s, 1, hr);
}
void pw_d3d9_draw_observe(struct pw_d3d9_draw_shadow *s, int nonnull, uint32_t hr)
{
    if (s && !hr) s->active = nonnull ? PW_D3D9_DECL_PRESENT : PW_D3D9_DECL_NULL;
}
void pw_d3d9_draw_begin(struct pw_d3d9_draw_shadow *s, uint32_t hr)
{
    if (!s || hr) return;
    s->recording = 1;
    memset(&s->pending, 0, sizeof(s->pending));
}
void pw_d3d9_draw_end(struct pw_d3d9_draw_shadow *s, struct pw_d3d9_draw_block *b, uint32_t hr)
{
    if (!s || !b || hr) return;
    *b = s->pending;
    s->recording = 0;
    memset(&s->pending, 0, sizeof(s->pending));
}
void pw_d3d9_draw_create_block(const struct pw_d3d9_draw_shadow *s,
        struct pw_d3d9_draw_block *b, uint32_t type, uint32_t hr)
{
    if (!s || !b || hr) return;
    b->captures_decl = type != 2;
    b->declaration = type >= 1 && type <= 3 ? s->active : PW_D3D9_DECL_UNKNOWN;
}
void pw_d3d9_draw_capture(const struct pw_d3d9_draw_shadow *s,
        struct pw_d3d9_draw_block *b, uint32_t hr)
{
    if (s && b && !hr && b->captures_decl) b->declaration = s->active;
}
void pw_d3d9_draw_apply(struct pw_d3d9_draw_shadow *s,
        const struct pw_d3d9_draw_block *b, uint32_t hr)
{
    if (!s || hr) return;
    if (!b) { pw_d3d9_draw_invalidate(s); return; }
    /* Pinned Apply does not call SetVertexDeclaration for a captured NULL. */
    if (b->captures_decl && b->declaration != PW_D3D9_DECL_NULL)
        s->active = b->declaration;
}
int pw_d3d9_draw_can_queue(const struct pw_d3d9_draw_shadow *s,
        const struct pw_d3d9_command *c)
{
    unsigned words, i;
    if (!s || !c || s->active != PW_D3D9_DECL_PRESENT || c->data_bytes) return 0;
    if (c->method == 81) words = 3;
    else if (c->method == 82) words = 6;
    else return 0;
    for (i = words; i < PW_D3D9_COMMAND_WORDS; ++i) if (c->args[i]) return 0;
    /* Defined primitive enums only. Native HRESULT validation has no additional
     * range checks, but unproven enum values remain synchronous. */
    return c->args[0] >= 1 && c->args[0] <= 6;
}
