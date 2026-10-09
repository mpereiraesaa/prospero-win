/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_TRANSFORM_OBSERVER_H
#define PW_D3D9_TRANSFORM_OBSERVER_H
#include <windows.h>
#include "../pw_d3d9_command_wire.h"
#include "../pw_d3d9_getter_wire.h"
#include "../pw_d3d9_transform_shadow.h"
#include "pw_d3d9_stateblock_client.h"
/* Negotiated HELLO capability for proxy-side Transform answers (client-only
 * behavior, but paired builds must agree on it). */
#define PW_D3D9_TRANSFORM_FEATURE 131072u
/* Typed adapters from validated wire outcomes to the transform shadow. Every
 * call runs under the original session admission gate, after the reply has
 * been fully decoded and before unlock; none allocates, locks or calls out.
 * Methods outside the Transform family are ignored. */
void pw_d3d9_transform_on_command(struct pw_d3d9_transform_shadow *,const struct pw_d3d9_command *,HRESULT);
void pw_d3d9_transform_on_getter(struct pw_d3d9_transform_shadow *,const struct pw_d3d9_getter_request *,const struct pw_d3d9_getter_reply *,HRESULT);
/* Every completed stateblock reply, including failures without payload. A
 * failed End may follow a native End that did complete, so recording becomes
 * unknown until a successful Begin/End/Create/Capture/Apply proves it again. */
void pw_d3d9_transform_on_stateblock(struct pw_d3d9_transform_shadow *,uint32_t method,uint32_t type,
 struct pw_d3d9_stateblock_evidence *,HRESULT);
/* Every completed Reset reply, whatever its result. */
void pw_d3d9_transform_on_reset(struct pw_d3d9_transform_shadow *);
/* Fills an owned reply for a known GetTransform; the caller copies it to the
 * application after unlocking. Returns 0 when the service must be asked. */
int pw_d3d9_transform_answer(const struct pw_d3d9_transform_shadow *,const struct pw_d3d9_getter_request *,struct pw_d3d9_getter_reply *);
/* For pw_d3d9_stateblock_client_commit invalidators: metadata only. */
void pw_d3d9_transform_invalidate_evidence(struct pw_d3d9_stateblock_evidence *);
#endif
