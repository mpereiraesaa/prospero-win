/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_KEY_SHARED_H
#define PW_KEY_SHARED_H
#include <stdint.h>
#include <stddef.h>
#define PW_KEY_QUERY 0x50574b01u
#define PW_KEY_VERSION 1u

/* Fixed ABI32 addresses. The provider validates complete read-only mappings,
 * and keeps session views alive until process teardown. */
struct pw_key_shared
{
    uint32_t version, input_object, desktop_object, reserved;
    uint64_t input_id, desktop_id;
    uint32_t state, lock, input_serial, desktop_serial;
};
_Static_assert(sizeof(struct pw_key_shared) == 48, "key descriptor ABI");
_Static_assert(offsetof(struct pw_key_shared, input_id) == 16, "key ID ABI");
_Static_assert(offsetof(struct pw_key_shared, state) == 32, "key addresses ABI");

/* The high/low/high read also works on PE32 without a writing cmpxchg8b on a
 * read-only server view. The surrounding object seqlock verifies consistency. */
static inline int pw_key_read64(uint32_t address, uint64_t *value)
{
    const volatile uint32_t *p = (const volatile uint32_t *)(uintptr_t)address;
    uint32_t hi = __atomic_load_n(p + 1, __ATOMIC_ACQUIRE);
    uint32_t lo = __atomic_load_n(p, __ATOMIC_ACQUIRE);
    if (hi != __atomic_load_n(p + 1, __ATOMIC_ACQUIRE)) return 0;
    *value = ((uint64_t)hi << 32) | lo;
    return 1;
}

static inline int pw_key_get_state(const struct pw_key_shared *d, int key, int16_t *out)
{
    unsigned attempt;
    if (d->version != PW_KEY_VERSION || d->reserved || !d->input_id || !d->desktop_id ||
        !d->input_object || !d->desktop_object || !d->state || !d->lock ||
        !d->input_serial || !d->desktop_serial ||
        (d->input_object & 7) || (d->desktop_object & 7) || (d->lock & 3) ||
        (d->input_serial & 7) || (d->desktop_serial & 7) ||
        d->input_object > UINT32_MAX - 16 || d->desktop_object > UINT32_MAX - 16 ||
        d->state > UINT32_MAX - 255 || d->lock > UINT32_MAX - 4 ||
        d->input_serial > UINT32_MAX - 8 || d->desktop_serial > UINT32_MAX - 8) return 0;
    for (attempt = 0; attempt < 3; ++attempt)
    {
        uint64_t seq, final, id, serial, desktop_seq = 0, desktop_serial = 0;
        uint8_t state;
        uint32_t locked;
        if (!pw_key_read64(d->input_object, &seq) || (seq & 1)) continue;
        if (!pw_key_read64(d->input_object + 8, &id) || id != d->input_id) return 0;
        state = ((const volatile uint8_t *)(uintptr_t)d->state)[key & 0xff];
        locked = __atomic_load_n((const volatile uint32_t *)(uintptr_t)d->lock, __ATOMIC_ACQUIRE);
        if (!pw_key_read64(d->input_serial, &serial)) continue;
        if (!locked)
        {
            if (!pw_key_read64(d->desktop_object, &desktop_seq) || (desktop_seq & 1)) continue;
            if (!pw_key_read64(d->desktop_object + 8, &id) || id != d->desktop_id) return 0;
            if (!pw_key_read64(d->desktop_serial, &desktop_serial)) continue;
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (!locked && (!pw_key_read64(d->desktop_object, &final) || final != desktop_seq)) continue;
        if (!pw_key_read64(d->input_object, &final) || final != seq) continue;
        if (!locked && serial != desktop_serial) return 0;
        *out = (int8_t)(state & 0x81);
        return 1;
    }
    return 0;
}
#endif
