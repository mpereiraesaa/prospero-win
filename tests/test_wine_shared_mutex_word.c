/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Native primitive tests. No Wine, game, signal/termination or fault workload. */
#include "ps5_mutex_word.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>

struct reference { uint32_t owner, count; };
struct modes {
    struct pw_mutex_word cell;
    struct reference legacy;
    unsigned slow, waiters, retired, activation_pin, owner_pin;
};

static void check(struct modes *m, const struct reference *r)
{
    uint64_t w = pw_mutex_word_load(&m->cell);
    assert(!!(w & PW_MUTEX_WORD_SLOW) == !!m->slow);
    if (m->slow) {
        assert(m->legacy.owner == r->owner && m->legacy.count == r->count);
        assert(m->owner_pin == !!r->count);
    } else {
        assert(pw_mutex_word_owner(w) == r->owner && pw_mutex_word_count(w) == r->count);
        assert(!m->legacy.owner && !m->legacy.count && !m->owner_pin);
    }
    assert(m->activation_pin);
}

static void enter(struct modes *m)
{
    uint64_t old;
    if (!pw_mutex_word_freeze(&m->cell, &old)) { assert(m->slow); return; }
    assert(!m->slow && !m->legacy.count && !m->owner_pin);
    m->slow = 1;
    m->legacy.owner = pw_mutex_word_owner(old);
    m->legacy.count = pw_mutex_word_count(old);
    m->owner_pin = !!m->legacy.count;
}

static int leave(struct modes *m)
{
    uint64_t desired;
    assert(m->slow);
    if (m->retired || m->waiters ||
        !pw_mutex_word_compose(m->legacy.owner, m->legacy.count, &desired)) return 0;
    /* The real caller must remove the legacy owner-list entry/reference
     * before publication. The activation pin prevents destruction here.
     */
    m->legacy = (struct reference){0, 0};
    m->owner_pin = 0;
    pw_mutex_word_publish(&m->cell, desired);
    m->slow = 0;
    return 1;
}

static void reference_take(struct reference *r, uint32_t token)
{
    assert(token && (!r->count || r->owner == token));
    r->owner = token; r->count++;
}
static void reference_release(struct reference *r, uint32_t token)
{
    assert(r->count && r->owner == token);
    if (!--r->count) r->owner = 0;
}

static void transitions(void)
{
    struct reference r = {0, 0};
    struct modes m = {.activation_pin = 1};
    unsigned checked = 0;
    for (uint32_t token = 1; token <= 3; token++) {
        for (unsigned round = 0; round < 40; round++) {
            for (unsigned depth = 0; depth < 8; depth++) {
                assert(pw_mutex_word_try_acquire(&m.cell, token));
                reference_take(&r, token); check(&m, &r); checked++;
                enter(&m); check(&m, &r); checked++;
                assert(leave(&m)); check(&m, &r); checked++;
            }
            for (unsigned depth = 8; depth; depth--) {
                uint32_t before = 0;
                assert(pw_mutex_word_try_release(&m.cell, token, &before));
                assert(before == r.count);
                reference_release(&r, token); check(&m, &r); checked++;
                enter(&m); check(&m, &r); checked++;
                assert(leave(&m)); check(&m, &r); checked++;
            }
        }
    }
    /* Ordinary queued handoff: server mode must persist until waiters drain. */
    assert(pw_mutex_word_try_acquire(&m.cell, 1)); reference_take(&r, 1);
    enter(&m); m.waiters = 1; assert(!leave(&m)); check(&m, &r);
    assert(!pw_mutex_word_try_acquire(&m.cell, 2));
    reference_release(&m.legacy, 1); reference_release(&r, 1); m.owner_pin = 0;
    reference_take(&m.legacy, 2); reference_take(&r, 2); m.owner_pin = 1;
    m.waiters = 0; assert(leave(&m)); check(&m, &r);
    uint32_t before;
    assert(pw_mutex_word_try_release(&m.cell, 2, &before) && before == 1);
    reference_release(&r, 2); check(&m, &r);

    /* A legal deep recursion continues in legacy mode, then recovers. */
    enter(&m); m.legacy = r = (struct reference){1, PW_MUTEX_WORD_MAX_COUNT}; m.owner_pin = 1;
    assert(leave(&m)); assert(!pw_mutex_word_try_acquire(&m.cell, 1));
    enter(&m); reference_take(&m.legacy, 1); reference_take(&r, 1);
    assert(!leave(&m)); check(&m, &r);
    reference_release(&m.legacy, 1); reference_release(&r, 1);
    assert(leave(&m)); check(&m, &r);

    /* Retirement freezes permanently; the server retains ordinary ownership. */
    enter(&m); m.retired = 1; assert(!leave(&m)); check(&m, &r);
    assert(!pw_mutex_word_try_release(&m.cell, 1, &before));
    puts("shared mutex word: independent recursion/mode/handoff/boundary/retirement checks PASS");
    printf("checked mode/operation observations: %u\n", checked);
}

static struct pw_mutex_word contended;
static atomic_uint inside;
static unsigned completed;
static void *worker(void *arg)
{
    uint32_t token = (uint32_t)(uintptr_t)arg;
    for (unsigned round = 0; round < 20000; round++) {
        unsigned attempts = 0;
        while (!pw_mutex_word_try_acquire(&contended, token)) {
            assert(++attempts < 10000000);
            sched_yield();
        }
        assert(atomic_fetch_add(&inside, 1) == 0);
        completed++;
        assert(atomic_fetch_sub(&inside, 1) == 1);
        uint32_t before = 0;
        assert(pw_mutex_word_try_release(&contended, token, &before) && before == 1);
    }
    return NULL;
}

int main(void)
{
    assert(sizeof(struct pw_mutex_word) == 64 && _Alignof(struct pw_mutex_word) == 64);
    assert(__atomic_is_lock_free(8, &contended.word));
    transitions();
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)(i + 1)));
    for (unsigned i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    assert(completed == 80000 && !atomic_load(&inside) && !pw_mutex_word_load(&contended));
    printf("shared mutex word: 4 live threads, %u legal critical sections, final free state PASS\n", completed);
    return 0;
}
