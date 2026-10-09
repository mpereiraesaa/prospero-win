/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "wine/ps5/pw_d3d9_objects.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    struct pw_d3d9_objects t;
    struct pw_d3d9_object_slot slots[2];
    struct pw_d3d9_object_ref a, b, found, stale;
    uintptr_t context = 0;
    uintptr_t high = (uintptr_t)UINT64_C(0x1000110c70);
    assert(!pw_d3d9_objects_init(&t, slots, 0, 1, 1));
    assert(!pw_d3d9_objects_init(&t, slots, 2, 0, 1));
    assert(!pw_d3d9_objects_init(&t, slots, 2, 1, 0));
    assert(pw_d3d9_objects_init(&t, slots, 2, 7, 9));
    assert(pw_d3d9_object_reserve(&t, &a));
    stale = a;
    assert(!pw_d3d9_object_commit(&t, a, 0, high, 1));
    assert(pw_d3d9_object_abort(&t, a));
    assert(!pw_d3d9_object_abort(&t, a));
    assert(pw_d3d9_object_reserve(&t, &a));
    assert(a.id == stale.id && a.generation == stale.generation + 1);
    assert(!pw_d3d9_object_commit(&t, stale, 42, high, 1));
    assert(pw_d3d9_object_commit(&t, a, 42, high, 1));
    assert(!pw_d3d9_object_lookup(&t, 8, 9, a));
    assert(!pw_d3d9_object_lookup(&t, 7, 10, a));
    assert(!pw_d3d9_object_lookup(&t, 7, 9, stale));
    assert(pw_d3d9_object_lookup(&t, 7, 9, a)->context == high);
    assert(pw_d3d9_object_find(&t, 42, &found));
    assert(found.id == a.id && found.generation == a.generation);
    assert(pw_d3d9_object_reserve(&t, &b));
    assert(!pw_d3d9_object_reserve(&t, &found));
    assert(!pw_d3d9_object_commit(&t, b, 42, high, 1));
    assert(pw_d3d9_object_abort(&t, b));
    slots[a.id - 1].guest_refs = UINT32_MAX;
    assert(!pw_d3d9_object_addref(&t, a, 0));
    slots[a.id - 1].guest_refs = 1;
    slots[a.id - 1].queued_refs = UINT64_MAX;
    assert(!pw_d3d9_object_queue(&t, a));
    slots[a.id - 1].queued_refs = 0;
    assert(!pw_d3d9_object_complete(&t, a));
    assert(pw_d3d9_object_queue(&t, a));
    assert(pw_d3d9_object_release(&t, a));
    assert(!pw_d3d9_object_release(&t, a));
    assert(!pw_d3d9_object_queue(&t, a));
    assert(!pw_d3d9_object_addref(&t, a, 0));
    assert(!pw_d3d9_object_take_destroy(&t, a, &context));
    assert(pw_d3d9_object_addref(&t, a, 1)); /* returned COM reference */
    assert(pw_d3d9_object_complete(&t, a));
    assert(!pw_d3d9_object_take_destroy(&t, a, &context));
    assert(pw_d3d9_object_release(&t, a));
    assert(pw_d3d9_object_take_destroy(&t, a, &context));
    assert(context == high);
    assert(!pw_d3d9_object_take_destroy(&t, a, &context));
    assert(!pw_d3d9_object_addref(&t, a, 1));
    assert(pw_d3d9_object_find(&t, 42, &found));
    assert(pw_d3d9_object_finish_destroy(&t, a));
    assert(!pw_d3d9_object_finish_destroy(&t, a));
    assert(!pw_d3d9_object_find(&t, 42, &found));
    assert(pw_d3d9_object_reserve(&t, &a));
    assert(pw_d3d9_object_commit(&t, a, 43, high, 1));
    assert(pw_d3d9_object_queue(&t, a));
    assert(pw_d3d9_object_reserve(&t, &b));
    pw_d3d9_objects_cancel(&t);
    pw_d3d9_objects_cancel(&t);
    assert(!pw_d3d9_object_commit(&t, b, 44, high, 1));
    assert(!pw_d3d9_object_reserve(&t, &b));
    assert(!pw_d3d9_object_addref(&t, a, 1));
    assert(!pw_d3d9_object_queue(&t, a));
    assert(!pw_d3d9_object_take_destroy(&t, a, &context));
    assert(pw_d3d9_object_complete(&t, a));
    assert(pw_d3d9_object_take_destroy(&t, a, &context));
    assert(pw_d3d9_object_finish_destroy(&t, a));
    assert(pw_d3d9_objects_init(&t, slots, 1, 7, 10));
    slots[0].generation = UINT32_MAX;
    assert(pw_d3d9_object_reserve(&t, &a));
    assert(pw_d3d9_object_abort(&t, a));
    assert(slots[0].state == PW_D3D9_EXHAUSTED);
    assert(!pw_d3d9_object_reserve(&t, &a));
    assert(!pw_d3d9_object_lookup(&t, 7, 10, a));
    puts("D3D9 identity/lifetime registry: PASS");
    return 0;
}
