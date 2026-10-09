/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_DRAW_SHADOW_H
#define PW_D3D9_DRAW_SHADOW_H
#include "pw_d3d9_command_wire.h"
/* Evidence only: never owns or fabricates a native resource. The caller holds
 * the same serialization lock for outcome updates and queue admission. */
enum pw_d3d9_decl_state {PW_D3D9_DECL_UNKNOWN, PW_D3D9_DECL_NULL, PW_D3D9_DECL_PRESENT};
struct pw_d3d9_draw_block {unsigned captures_decl; enum pw_d3d9_decl_state declaration;};
struct pw_d3d9_draw_shadow {
    enum pw_d3d9_decl_state active;
    unsigned recording;
    struct pw_d3d9_draw_block pending;
};
void pw_d3d9_draw_init(struct pw_d3d9_draw_shadow *);
void pw_d3d9_draw_invalidate(struct pw_d3d9_draw_shadow *);
void pw_d3d9_draw_declaration(struct pw_d3d9_draw_shadow *, int nonnull, uint32_t hr);
void pw_d3d9_draw_fvf(struct pw_d3d9_draw_shadow *, uint32_t fvf, uint32_t hr);
void pw_d3d9_draw_observe(struct pw_d3d9_draw_shadow *, int nonnull, uint32_t hr);
void pw_d3d9_draw_begin(struct pw_d3d9_draw_shadow *, uint32_t hr);
void pw_d3d9_draw_end(struct pw_d3d9_draw_shadow *, struct pw_d3d9_draw_block *, uint32_t hr);
void pw_d3d9_draw_create_block(const struct pw_d3d9_draw_shadow *, struct pw_d3d9_draw_block *, uint32_t type, uint32_t hr);
void pw_d3d9_draw_capture(const struct pw_d3d9_draw_shadow *, struct pw_d3d9_draw_block *, uint32_t hr);
void pw_d3d9_draw_apply(struct pw_d3d9_draw_shadow *, const struct pw_d3d9_draw_block *, uint32_t hr);
int pw_d3d9_draw_can_queue(const struct pw_d3d9_draw_shadow *, const struct pw_d3d9_command *);
#endif
