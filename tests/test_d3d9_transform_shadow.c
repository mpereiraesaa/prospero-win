/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_transform_shadow.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define BAD 0x8876086cu /* D3DERR_INVALIDCALL */
#define FAIL 0x80004005u /* E_FAIL: outcome unknown */
#define VIEW 2u
#define PROJECTION 3u
#define WORLD 256u
static void matrix(unsigned char out[64], unsigned seed)
{
    for (unsigned i = 0; i < 64; i++) out[i] = (unsigned char)(seed * 31u + i * 7u);
}
static int has(const struct pw_d3d9_transform_shadow *s, uint32_t state, const unsigned char want[64])
{
    unsigned char got[64];
    memset(got, 0xcd, sizeof(got));
    if (!pw_d3d9_transform_lookup(s, state, got)) return 0;
    assert(!memcmp(got, want, 64));
    return 1;
}
static int unknown(const struct pw_d3d9_transform_shadow *s, uint32_t state)
{
    unsigned char got[64], poison[64];
    memset(got, 0xcd, sizeof(got)); memset(poison, 0xcd, sizeof(poison));
    int hit = pw_d3d9_transform_lookup(s, state, got);
    assert(hit || !memcmp(got, poison, 64)); /* a miss writes nothing */
    return !hit;
}
int main(void)
{
    struct pw_d3d9_transform_shadow *s = malloc(sizeof(*s));
    unsigned char a[64], b[64], c[64], d[64], id[64];
    const float one[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    unsigned slots = 0;
    assert(s);
    matrix(a, 1); matrix(b, 2); matrix(c, 3); matrix(d, 4); memcpy(id, one, 64);
    for (uint32_t state = 0; state < 1024; state++) {
        int slot = pw_d3d9_transform_slot(state);
        if (slot < 0) continue;
        assert((unsigned)slot < PW_D3D9_TRANSFORM_SLOTS);
        slots++;
    }
    assert(slots == PW_D3D9_TRANSFORM_SLOTS);
    assert(pw_d3d9_transform_slot(2) == 0 && pw_d3d9_transform_slot(3) == 1 && pw_d3d9_transform_slot(16) == 2 &&
           pw_d3d9_transform_slot(23) == 9 && pw_d3d9_transform_slot(256) == 10 && pw_d3d9_transform_slot(511) == 265);
    assert(pw_d3d9_transform_slot(0) < 0 && pw_d3d9_transform_slot(1) < 0 && pw_d3d9_transform_slot(4) < 0 &&
           pw_d3d9_transform_slot(15) < 0 && pw_d3d9_transform_slot(24) < 0 && pw_d3d9_transform_slot(255) < 0 &&
           pw_d3d9_transform_slot(512) < 0 && pw_d3d9_transform_slot(UINT32_MAX) < 0);

    /* Fresh device: nothing is assumed, not even identity. */
    pw_d3d9_transform_init(s);
    assert(unknown(s, VIEW) && unknown(s, WORLD) && unknown(s, 511));
    assert(!pw_d3d9_transform_lookup(s, VIEW, NULL));

    /* Exact S_OK Set and Get outcomes, NULL as identity, failures forget. */
    pw_d3d9_transform_set(s, VIEW, a, 0); assert(has(s, VIEW, a));
    pw_d3d9_transform_set(s, VIEW, b, BAD); assert(unknown(s, VIEW));
    pw_d3d9_transform_set(s, PROJECTION, NULL, 0); assert(has(s, PROJECTION, id));
    pw_d3d9_transform_observe(s, VIEW, c, BAD); assert(unknown(s, VIEW));
    pw_d3d9_transform_observe(s, VIEW, c, 0); assert(has(s, VIEW, c));
    pw_d3d9_transform_observe(s, 1, c, 0); assert(unknown(s, 1));
    pw_d3d9_transform_multiply(s, VIEW); assert(unknown(s, VIEW) && has(s, PROJECTION, id));
    pw_d3d9_transform_set(s, VIEW, a, 0);
    pw_d3d9_transform_set(s, 4, b, 0); /* unbounded backend index: forget everything */
    assert(unknown(s, VIEW) && unknown(s, PROJECTION));
    pw_d3d9_transform_set(s, VIEW, a, 0); pw_d3d9_transform_multiply(s, 1);
    assert(unknown(s, VIEW));

    /* Recording leaves live values alone; End publishes exactly what was recorded. */
    struct pw_d3d9_transform_block rec = {0}, all = {0}, pixel = {0}, vertex = {0}, failed = {0}, odd = {0}, oom = {0}, other = {0};
    pw_d3d9_transform_set(s, VIEW, a, 0); pw_d3d9_transform_set(s, WORLD, a, 0);
    pw_d3d9_transform_begin(s, 0);
    pw_d3d9_transform_set(s, VIEW, b, 0); pw_d3d9_transform_set(s, PROJECTION, NULL, 0);
    assert(has(s, VIEW, a) && unknown(s, PROJECTION));
    pw_d3d9_transform_multiply(s, WORLD); assert(unknown(s, WORLD)); /* not recorded, live changed */
    pw_d3d9_transform_capture(s, &other, BAD); pw_d3d9_transform_apply(s, &other, BAD); /* refused while recording */
    assert(has(s, VIEW, a) && s->recording == PW_D3D9_TRANSFORM_RECORDING);
    pw_d3d9_transform_apply(s, &other, FAIL); assert(unknown(s, VIEW) && s->recording == PW_D3D9_TRANSFORM_RECORDING);
    pw_d3d9_transform_set(s, VIEW, a, 0); /* still recording: does not reach live */
    pw_d3d9_transform_block_prepare(&rec);
    pw_d3d9_transform_end(s, &rec, 0);
    assert(rec.known && unknown(s, VIEW) && unknown(s, PROJECTION));
    pw_d3d9_transform_set(s, VIEW, c, 0); pw_d3d9_transform_set(s, PROJECTION, c, 0); pw_d3d9_transform_set(s, WORLD, c, 0);
    pw_d3d9_transform_apply(s, &rec, 0);
    assert(has(s, VIEW, a) && has(s, PROJECTION, id) && has(s, WORLD, c)); /* only recorded slots move */

    /* Capture refreshes only captured slots; unknown live values stay unknown. */
    pw_d3d9_transform_set(s, VIEW, d, 0); pw_d3d9_transform_multiply(s, PROJECTION);
    pw_d3d9_transform_capture(s, &rec, 0);
    pw_d3d9_transform_set(s, VIEW, b, 0); pw_d3d9_transform_set(s, PROJECTION, b, 0);
    pw_d3d9_transform_apply(s, &rec, 0);
    assert(has(s, VIEW, d) && unknown(s, PROJECTION) && has(s, WORLD, c));
    pw_d3d9_transform_set(s, PROJECTION, b, 0);
    pw_d3d9_transform_capture(s, &rec, FAIL); /* may have captured: forget the block values */
    pw_d3d9_transform_apply(s, &rec, 0);
    assert(unknown(s, VIEW) && unknown(s, PROJECTION) && has(s, WORLD, c));

    /* D3DSBT_ALL snapshots everything incl. unknowns; pixel/vertex capture none. */
    pw_d3d9_transform_set(s, VIEW, a, 0); pw_d3d9_transform_set(s, 511, b, 0);
    pw_d3d9_transform_block_prepare(&all); pw_d3d9_transform_create(s, &all, 1, 0);
    pw_d3d9_transform_create(s, &pixel, 2, 0);
    pw_d3d9_transform_block_prepare(&vertex); pw_d3d9_transform_create(s, &vertex, 3, 0);
    pw_d3d9_transform_block_prepare(&failed); pw_d3d9_transform_create(s, &failed, 1, BAD);
    assert(all.known && pixel.known && !pixel.set && vertex.known && !failed.known);
    pw_d3d9_transform_set(s, VIEW, c, 0); pw_d3d9_transform_set(s, PROJECTION, c, 0); pw_d3d9_transform_set(s, 511, c, 0);
    pw_d3d9_transform_apply(s, &pixel, 0); pw_d3d9_transform_apply(s, &vertex, 0);
    pw_d3d9_transform_capture(s, &pixel, 0); pw_d3d9_transform_capture(s, &vertex, 0);
    assert(has(s, VIEW, c) && has(s, PROJECTION, c) && has(s, 511, c));
    pw_d3d9_transform_apply(s, &all, 0);
    assert(has(s, VIEW, a) && unknown(s, PROJECTION) && has(s, 511, b) && has(s, WORLD, c));
    pw_d3d9_transform_set(s, PROJECTION, d, 0);
    pw_d3d9_transform_capture(s, &all, 0);
    pw_d3d9_transform_set(s, PROJECTION, a, 0);
    pw_d3d9_transform_apply(s, &all, 0); assert(has(s, PROJECTION, d) && has(s, VIEW, a));
    pw_d3d9_transform_apply(s, &failed, 0); /* unknown block */
    assert(unknown(s, VIEW) && unknown(s, PROJECTION) && unknown(s, WORLD));
    pw_d3d9_transform_set(s, VIEW, a, 0); pw_d3d9_transform_apply(s, NULL, 0); assert(unknown(s, VIEW));
    pw_d3d9_transform_set(s, VIEW, a, 0);
    pw_d3d9_transform_apply(s, &all, BAD); assert(has(s, VIEW, a) && s->recording == PW_D3D9_TRANSFORM_RECORDING_UNKNOWN);
    pw_d3d9_transform_capture(s, NULL, 0);
    pw_d3d9_transform_capture(s, &all, BAD); assert(s->recording == PW_D3D9_TRANSFORM_RECORDING_UNKNOWN);
    pw_d3d9_transform_capture(s, NULL, 0);
    pw_d3d9_transform_apply(s, &all, FAIL); assert(unknown(s, VIEW)); /* failed Apply may have applied */
    pw_d3d9_transform_block_prepare(&odd); pw_d3d9_transform_create(s, &odd, 9, 0); assert(!odd.known);
    pw_d3d9_transform_set(s, VIEW, a, 0); pw_d3d9_transform_apply(s, &odd, 0); assert(unknown(s, VIEW));
    pw_d3d9_transform_create(s, &oom, 1, 0); assert(!oom.known); /* no storage: unknown, not an error */
    pw_d3d9_transform_begin(s, 0); pw_d3d9_transform_end(s, &oom, 0); assert(!oom.known && s->recording == PW_D3D9_TRANSFORM_LIVE);
    pw_d3d9_transform_block_prepare(&all); assert(!all.known); /* reuse forgets old evidence */
    pw_d3d9_transform_set(s, VIEW, a, 0); pw_d3d9_transform_apply(s, &all, 0); assert(unknown(s, VIEW));

    /* Unknown recording state: setters forget, the recorded block is unknown. */
    pw_d3d9_transform_set(s, VIEW, a, 0); pw_d3d9_transform_set(s, PROJECTION, a, 0);
    pw_d3d9_transform_begin(s, BAD);
    assert(s->recording == PW_D3D9_TRANSFORM_RECORDING_UNKNOWN && has(s, VIEW, a));
    pw_d3d9_transform_set(s, VIEW, b, 0); assert(unknown(s, VIEW) && has(s, PROJECTION, a));
    pw_d3d9_transform_block_prepare(&rec); pw_d3d9_transform_end(s, &rec, 0);
    assert(!rec.known && s->recording == PW_D3D9_TRANSFORM_LIVE);
    pw_d3d9_transform_set(s, VIEW, b, 0); pw_d3d9_transform_apply(s, &rec, 0);
    assert(unknown(s, VIEW) && unknown(s, PROJECTION));
    pw_d3d9_transform_begin(s, 0); pw_d3d9_transform_set(s, VIEW, a, 0); pw_d3d9_transform_set(s, 300, a, BAD);
    pw_d3d9_transform_block_prepare(&rec); pw_d3d9_transform_end(s, &rec, 0); /* a failed recorded Set taints the block */
    assert(!rec.known);
    pw_d3d9_transform_begin(s, 0); pw_d3d9_transform_set(s, 4, a, 0);
    pw_d3d9_transform_block_prepare(&rec); pw_d3d9_transform_end(s, &rec, 0); assert(!rec.known);
    pw_d3d9_transform_begin(s, 0); pw_d3d9_transform_end(s, &rec, BAD);
    assert(s->recording == PW_D3D9_TRANSFORM_RECORDING_UNKNOWN);
    pw_d3d9_transform_set(s, VIEW, a, 0); assert(unknown(s, VIEW));
    pw_d3d9_transform_capture(s, NULL, 0); assert(s->recording == PW_D3D9_TRANSFORM_LIVE); /* Capture proves live */
    pw_d3d9_transform_set(s, VIEW, a, 0); assert(has(s, VIEW, a));
    pw_d3d9_transform_begin(s, BAD); pw_d3d9_transform_create(s, &pixel, 2, 0);
    assert(s->recording == PW_D3D9_TRANSFORM_LIVE);
    pw_d3d9_transform_begin(s, BAD); pw_d3d9_transform_apply(s, &pixel, 0);
    assert(s->recording == PW_D3D9_TRANSFORM_LIVE && has(s, VIEW, a));
    pw_d3d9_transform_begin(s, BAD); pw_d3d9_transform_capture(s, &pixel, BAD);
    assert(s->recording == PW_D3D9_TRANSFORM_RECORDING_UNKNOWN);

    /* Reset boundaries invalidate live values; snapshots and recording state stay. */
    pw_d3d9_transform_capture(s, NULL, 0);
    pw_d3d9_transform_block_prepare(&all); pw_d3d9_transform_create(s, &all, 1, 0);
    pw_d3d9_transform_begin(s, 0);
    pw_d3d9_transform_invalidate(s); assert(unknown(s, VIEW) && s->recording == PW_D3D9_TRANSFORM_RECORDING);
    pw_d3d9_transform_block_prepare(&rec); pw_d3d9_transform_end(s, &rec, 0);
    pw_d3d9_transform_apply(s, &all, 0); assert(has(s, VIEW, a));

    pw_d3d9_transform_block_free(&rec); pw_d3d9_transform_block_free(&all); pw_d3d9_transform_block_free(&pixel);
    pw_d3d9_transform_block_free(&vertex); pw_d3d9_transform_block_free(&failed); pw_d3d9_transform_block_free(&odd);
    pw_d3d9_transform_block_free(&oom); pw_d3d9_transform_block_free(&other); pw_d3d9_transform_block_free(NULL);
    assert(!rec.set && !rec.known);
    free(s);
    puts("PASS d3d9 transform shadow");
    return 0;
}
