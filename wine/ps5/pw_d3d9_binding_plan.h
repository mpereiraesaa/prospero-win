/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_BINDING_PLAN_H
#define PW_D3D9_BINDING_PLAN_H
/* Additional HELLO capability; scalar batch support alone never admits bindings. */
#define PW_D3D9_BINDING_FEATURE 32768u
#include "pw_d3d9_command_wire.h"
enum pw_d3d9_binding_status {
    PW_D3D9_BINDING_OTHER, PW_D3D9_BINDING_READY,
    PW_D3D9_BINDING_INVALID, PW_D3D9_BINDING_SYNCHRONOUS
};
struct pw_d3d9_binding {
    uint32_t method, kind, object_word, id, generation;
};
/* Pinned DXVK5fde scalar/canonical DTO contract ONLY. READY is not permission
 * to enqueue: validate/pin the exact typed object, same device/session/epoch,
 * generation, device lifetime and Release barriers separately. No guest pointer
 * is read. Null is exactly {0,0}; an output is written only for READY.
 * INVALID maps to INVALIDCALL; OTHER/SYNCHRONOUS retain the existing sync path.
 * Texture scope is currently Texture2D. Cube/volume bindings require a separate
 * typed contract. The generic queue policy continues rejecting object words. */
int pw_d3d9_binding_plan(const struct pw_d3d9_command *,struct pw_d3d9_binding *);
#endif
