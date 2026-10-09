/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_OBJECTS_H
#define PW_D3D9_OBJECTS_H
#include <stdint.h>
#include <stddef.h>

/* Local bookkeeping only. Serialize every operation with the adapter's lock.
 * Backend pointers never enter the transport. Caller owns storage and COM refs. */
struct pw_d3d9_object_ref { uint32_t id, generation; };
enum pw_d3d9_object_state { PW_D3D9_FREE, PW_D3D9_RESERVED, PW_D3D9_LIVE,
    PW_D3D9_RETIRING, PW_D3D9_DESTROYING, PW_D3D9_EXHAUSTED };
struct pw_d3d9_object_slot {
    uint32_t generation, guest_refs, kind;
    uint64_t queued_refs;
    uintptr_t identity, context;
    enum pw_d3d9_object_state state;
};
struct pw_d3d9_objects {
    struct pw_d3d9_object_slot *slots;
    uint32_t capacity, device, epoch;
    int cancelled;
};
int pw_d3d9_objects_init(struct pw_d3d9_objects *, struct pw_d3d9_object_slot *,
    uint32_t capacity, uint32_t device, uint32_t epoch);
int pw_d3d9_object_reserve(struct pw_d3d9_objects *, struct pw_d3d9_object_ref *);
int pw_d3d9_object_commit(struct pw_d3d9_objects *, struct pw_d3d9_object_ref,
    uintptr_t identity, uintptr_t context, uint32_t kind);
int pw_d3d9_object_abort(struct pw_d3d9_objects *, struct pw_d3d9_object_ref);
/* Finds canonical IUnknown identity, including retiring/destroying entries.
 * No reference is acquired. A returned COM reference may explicitly revive a
 * retiring entry; a destroying entry must finish destruction first. */
int pw_d3d9_object_find(const struct pw_d3d9_objects *, uintptr_t identity,
    struct pw_d3d9_object_ref *);
int pw_d3d9_object_addref(struct pw_d3d9_objects *, struct pw_d3d9_object_ref,
    int returned_reference);
int pw_d3d9_object_release(struct pw_d3d9_objects *, struct pw_d3d9_object_ref);
int pw_d3d9_object_queue(struct pw_d3d9_objects *, struct pw_d3d9_object_ref);
int pw_d3d9_object_complete(struct pw_d3d9_objects *, struct pw_d3d9_object_ref);
/* take_destroy transfers the local backend-release obligation to the caller.
 * Release COM outside the lock, then finish_destroy under the lock. */
int pw_d3d9_object_take_destroy(struct pw_d3d9_objects *, struct pw_d3d9_object_ref,
    uintptr_t *context);
int pw_d3d9_object_finish_destroy(struct pw_d3d9_objects *, struct pw_d3d9_object_ref);
void pw_d3d9_objects_cancel(struct pw_d3d9_objects *);
const struct pw_d3d9_object_slot *pw_d3d9_object_lookup(const struct pw_d3d9_objects *,
    uint32_t device, uint32_t epoch, struct pw_d3d9_object_ref);
#endif
