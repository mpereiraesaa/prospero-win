/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_objects.h"
#include <string.h>

static struct pw_d3d9_object_slot *slot(const struct pw_d3d9_objects *t,
                                      struct pw_d3d9_object_ref r)
{
    struct pw_d3d9_object_slot *s;
    if (!r.id || r.id > t->capacity || !r.generation) return NULL;
    s = &t->slots[r.id - 1];
    if (s->generation != r.generation || s->state == PW_D3D9_FREE ||
        s->state == PW_D3D9_EXHAUSTED) return NULL;
    return s;
}
static void recycle(struct pw_d3d9_object_slot *s)
{
    uint32_t generation = s->generation;
    memset(s, 0, sizeof(*s));
    s->generation = generation == UINT32_MAX ? generation : generation + 1;
    s->state = generation == UINT32_MAX ? PW_D3D9_EXHAUSTED : PW_D3D9_FREE;
}
int pw_d3d9_objects_init(struct pw_d3d9_objects *t, struct pw_d3d9_object_slot *s,
                       uint32_t capacity, uint32_t device, uint32_t epoch)
{
    uint32_t i;
    if (!t || !s || !capacity || !device || !epoch ||
        (uint64_t)capacity * sizeof(*s) > SIZE_MAX) return 0;
    memset(s, 0, (size_t)capacity * sizeof(*s));
    for (i = 0; i < capacity; ++i) s[i].generation = 1;
    *t = (struct pw_d3d9_objects){s, capacity, device, epoch, 0};
    return 1;
}
int pw_d3d9_object_reserve(struct pw_d3d9_objects *t, struct pw_d3d9_object_ref *r)
{
    uint32_t i;
    if (t->cancelled || !r) return 0;
    for (i = 0; i < t->capacity; ++i) if (t->slots[i].state == PW_D3D9_FREE) {
        t->slots[i].state = PW_D3D9_RESERVED;
        *r = (struct pw_d3d9_object_ref){i + 1, t->slots[i].generation};
        return 1;
    }
    return 0;
}
int pw_d3d9_object_find(const struct pw_d3d9_objects *t, uintptr_t identity,
                       struct pw_d3d9_object_ref *r)
{
    uint32_t i;
    if (!identity || !r) return 0;
    for (i = 0; i < t->capacity; ++i) {
        const struct pw_d3d9_object_slot *s = &t->slots[i];
        if (s->identity == identity && (s->state == PW_D3D9_LIVE ||
            s->state == PW_D3D9_RETIRING || s->state == PW_D3D9_DESTROYING)) {
            *r = (struct pw_d3d9_object_ref){i + 1, s->generation};
            return 1;
        }
    }
    return 0;
}
int pw_d3d9_object_commit(struct pw_d3d9_objects *t, struct pw_d3d9_object_ref r,
                         uintptr_t identity, uintptr_t context, uint32_t kind)
{
    struct pw_d3d9_object_ref existing;
    struct pw_d3d9_object_slot *s = slot(t, r);
    if (t->cancelled || !s || s->state != PW_D3D9_RESERVED || !identity || !kind ||
        pw_d3d9_object_find(t, identity, &existing)) return 0;
    s->identity = identity; s->context = context; s->kind = kind;
    s->guest_refs = 1; s->state = PW_D3D9_LIVE;
    return 1;
}
int pw_d3d9_object_abort(struct pw_d3d9_objects *t, struct pw_d3d9_object_ref r)
{
    struct pw_d3d9_object_slot *s = slot(t, r);
    if (!s || s->state != PW_D3D9_RESERVED) return 0;
    recycle(s); return 1;
}
int pw_d3d9_object_addref(struct pw_d3d9_objects *t, struct pw_d3d9_object_ref r,
                         int returned_reference)
{
    struct pw_d3d9_object_slot *s = slot(t, r);
    if (t->cancelled || !s || s->guest_refs == UINT32_MAX ||
        (s->state != PW_D3D9_LIVE &&
         !(returned_reference && s->state == PW_D3D9_RETIRING))) return 0;
    ++s->guest_refs; s->state = PW_D3D9_LIVE; return 1;
}
int pw_d3d9_object_release(struct pw_d3d9_objects *t, struct pw_d3d9_object_ref r)
{
    struct pw_d3d9_object_slot *s = slot(t, r);
    if (!s || s->state != PW_D3D9_LIVE || !s->guest_refs) return 0;
    if (!--s->guest_refs) s->state = PW_D3D9_RETIRING;
    return 1;
}
int pw_d3d9_object_queue(struct pw_d3d9_objects *t, struct pw_d3d9_object_ref r)
{
    struct pw_d3d9_object_slot *s = slot(t, r);
    if (t->cancelled || !s || s->state != PW_D3D9_LIVE || s->queued_refs == UINT64_MAX)
        return 0;
    ++s->queued_refs; return 1;
}
int pw_d3d9_object_complete(struct pw_d3d9_objects *t, struct pw_d3d9_object_ref r)
{
    struct pw_d3d9_object_slot *s = slot(t, r);
    if (!s || (s->state != PW_D3D9_LIVE && s->state != PW_D3D9_RETIRING) ||
        !s->queued_refs) return 0;
    --s->queued_refs; return 1;
}
int pw_d3d9_object_take_destroy(struct pw_d3d9_objects *t, struct pw_d3d9_object_ref r,
                               uintptr_t *context)
{
    struct pw_d3d9_object_slot *s = slot(t, r);
    if (!s || !context || s->state != PW_D3D9_RETIRING || s->queued_refs) return 0;
    *context = s->context; s->state = PW_D3D9_DESTROYING; return 1;
}
int pw_d3d9_object_finish_destroy(struct pw_d3d9_objects *t, struct pw_d3d9_object_ref r)
{
    struct pw_d3d9_object_slot *s = slot(t, r);
    if (!s || s->state != PW_D3D9_DESTROYING) return 0;
    recycle(s); return 1;
}
void pw_d3d9_objects_cancel(struct pw_d3d9_objects *t)
{
    uint32_t i;
    t->cancelled = 1;
    for (i = 0; i < t->capacity; ++i) {
        struct pw_d3d9_object_slot *s = &t->slots[i];
        if (s->state == PW_D3D9_RESERVED) recycle(s);
        else if (s->state == PW_D3D9_LIVE) {
            s->guest_refs = 0; s->state = PW_D3D9_RETIRING;
        }
    }
}
const struct pw_d3d9_object_slot *pw_d3d9_object_lookup(const struct pw_d3d9_objects *t,
    uint32_t device, uint32_t epoch, struct pw_d3d9_object_ref r)
{
    if (t->device != device || t->epoch != epoch) return NULL;
    return slot(t, r);
}
