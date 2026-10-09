/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_BINDING_LEASES_H
#define PW_D3D9_BINDING_LEASES_H
#include "pw_d3d9_native_command.h"
#include "../pw_d3d9_command_batch.h"
#include "../pw_d3d9_binding_plan.h"
#include "../pw_d3d9_objects.h"
struct pw_d3d9_binding_lease {
    struct pw_d3d9_binding binding;
    IDirect3DDevice9 *device;
    IUnknown *native;
    HRESULT result;
    uint32_t unexpected_result;
    int queued;
};
struct pw_d3d9_binding_leases {
    struct pw_d3d9_command_batch batch;
    struct pw_d3d9_binding_lease records[PW_D3D9_BATCH_LIMIT];
    struct pw_d3d9_objects *objects;
    int prepared;
};
/* Zero-initialize first. Caller serializes registry access and holds the target
 * device generation pin until AFTER release. No registry mutex may span native
 * COM calls. Own/validate all bytes and policy before taking any resource pins.
 * Successful preparation does NOT mean every record's typed target is valid:
 * saved target failures surface at their ordered execution position, preserving
 * the exact first-failure prefix. The client must reject ordinary invalid inputs
 * before asynchronous acceptance; these saved errors are channel-fatal fallback.
 * Execute only commands from this owned batch; never reread the caller's input.
 * No shipping queue policy or transport is enabled by this helper. */
int pw_d3d9_binding_leases_prepare(struct pw_d3d9_binding_leases *,
    struct pw_d3d9_objects *,IDirect3DDevice9 *,const void *,size_t,
    pw_d3d9_command_acquire_fn,void *);
/* Draw support is separately negotiated. This variant admits only structurally
 * valid draw records; the caller must prove ordered native declaration state
 * after all leases are prepared and before executing the first command. */
int pw_d3d9_binding_leases_prepare_draws(struct pw_d3d9_binding_leases *,
    struct pw_d3d9_objects *,IDirect3DDevice9 *,const void *,size_t,
    pw_d3d9_command_acquire_fn,void *,int);
/* Pass &leases.records[command_index] to native_command_dispatch's acquire.
 * Returns one owned typed COM reference, as its existing contract requires. */
HRESULT pw_d3d9_binding_lease_acquire(void *,uint32_t,uint32_t,uint32_t,IDirect3DDevice9 *,void **);
/* All success/failure/cancel paths must release, including unexecuted suffixes.
 * Registry destruction can drain only after these generation pins are dropped. */
int pw_d3d9_binding_leases_release(struct pw_d3d9_binding_leases *);
#endif
