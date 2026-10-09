/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_STATEBLOCK_CLIENT_H
#define PW_D3D9_STATEBLOCK_CLIENT_H
#include <d3d9.h>
#include "pw_d3d9_session.h"
#include "../pw_d3d9_stateblock_wire.h"
#include "../pw_d3d9_draw_shadow.h"
/* Owned by the preallocated COM shell. Extend this aggregate for additional
 * state families; it is local metadata, never a wire object or an ID registry. */
struct pw_d3d9_stateblock_evidence {struct pw_d3d9_draw_block draw;};
struct pw_d3d9_stateblock_client_ops {
 HRESULT (*call)(IDirect3DDevice9 *,struct pw_d3d9_object_ref,const struct pw_d3d9_stateblock_request *,struct pw_d3d9_stateblock_reply *);
 HRESULT (*release)(IDirect3DDevice9 *,struct pw_d3d9_object_ref);
 HRESULT (*defer)(IDirect3DDevice9 *,struct pw_d3d9_deferred *);
 void (*fail)(IDirect3DDevice9 *,HRESULT);
#ifdef PW_D3D9_ENABLE_STATE_EVIDENCE
 /* Must validate the typed reply and update parent/block evidence under the
  * session gate before unlocking. No callback may retain the evidence pointer.
  * Create/End must commit validated output with the helper below while still
  * holding the original gate. Capture/Apply pass pinned owned
  * storage; Begin passes NULL. No RPC/COM call while updating evidence. */
 HRESULT (*observed_call)(IDirect3DDevice9 *,struct pw_d3d9_object_ref,
  const struct pw_d3d9_stateblock_request *,struct pw_d3d9_stateblock_reply *,
  struct pw_d3d9_stateblock_evidence *);
#endif
};
/* Install once before vtable publication. Ref0/0 targets the parent device.
 * fail cancels/sticks the session; native objects then belong to service teardown.
 * Failed deferred enqueue retains local ownership; never joins inside reentry. */
#ifdef PW_D3D9_ENABLE_STATE_EVIDENCE
/* Call only after full reply validation, inside the original admitted gate.
 * Canonicalization publishes preallocated storage to the cache atomically with
 * evidence. invalidate runs under gate+cache_lock and must only mutate metadata:
 * no RPC, COM, allocation or lock acquisition. Later failure rolls back the
 * caller's ownership outside the gate. No pointer is returned without a pin. */
HRESULT pw_d3d9_stateblock_client_commit(struct pw_d3d9_stateblock_evidence *,
 struct pw_d3d9_object_ref,void (*invalidate)(struct pw_d3d9_stateblock_evidence *));
#endif
void pw_d3d9_stateblock_client_install(IDirect3DDevice9Vtbl *,const struct pw_d3d9_stateblock_client_ops *);
#endif
