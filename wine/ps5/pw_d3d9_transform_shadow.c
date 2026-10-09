/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_transform_shadow.h"
#include <stdlib.h>
#include <string.h>
#define REFUSED 0x8876086cu /* D3DERR_INVALIDCALL: refused before any effect */
static const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
static int test_bit(const uint32_t *bits, unsigned n) { return (bits[n / 32] >> (n % 32)) & 1; }
static void put_bit(uint32_t *bits, unsigned n, int on)
{
    if (on) bits[n / 32] |= 1u << (n % 32); else bits[n / 32] &= ~(1u << (n % 32));
}
int pw_d3d9_transform_slot(uint32_t state)
{
    if (state == 2) return 0;                          /* D3DTS_VIEW */
    if (state == 3) return 1;                          /* D3DTS_PROJECTION */
    if (state >= 16 && state <= 23) return (int)(state - 14); /* TEXTURE0..7 */
    if (state >= 256 && state <= 511) return (int)(state - 246); /* WORLDMATRIX(0..255) */
    return -1; /* The backend indexes these without a bound; never model them. */
}
void pw_d3d9_transform_init(struct pw_d3d9_transform_shadow *s)
{
    if (s) memset(s, 0, sizeof(*s));
}
void pw_d3d9_transform_invalidate(struct pw_d3d9_transform_shadow *s)
{
    if (s) memset(s->live.known, 0, sizeof(s->live.known));
}
/* The recording domain is no longer known: serve nothing until a successful
 * Begin or End establishes it again. */
static void uncertain(struct pw_d3d9_transform_shadow *s)
{
    s->recording = PW_D3D9_TRANSFORM_RECORDING_UNKNOWN;
    s->pending_unknown = 1;
    memset(s->live.known, 0, sizeof(s->live.known));
}
static void forget_pending(struct pw_d3d9_transform_shadow *s)
{
    memset(s->pending.captured, 0, sizeof(s->pending.captured));
    memset(s->pending.known, 0, sizeof(s->pending.known));
    s->pending_unknown = 0;
}
void pw_d3d9_transform_set(struct pw_d3d9_transform_shadow *s, uint32_t state, const void *matrix, uint32_t hr)
{
    int slot = pw_d3d9_transform_slot(state);
    if (!s) return;
    if (slot < 0) { pw_d3d9_transform_invalidate(s); s->pending_unknown = 1; return; }
    if (!matrix) matrix = identity;
    if (hr || s->recording == PW_D3D9_TRANSFORM_RECORDING_UNKNOWN) {
        /* Unknown outcome or destination: it may have reached either. */
        put_bit(s->live.known, (unsigned)slot, 0);
        s->pending_unknown = 1;
        return;
    }
    struct pw_d3d9_transform_set *target = s->recording == PW_D3D9_TRANSFORM_RECORDING ? &s->pending : &s->live;
    memcpy(target->value[slot], matrix, 64);
    put_bit(target->known, (unsigned)slot, 1);
    put_bit(target->captured, (unsigned)slot, 1);
}
void pw_d3d9_transform_multiply(struct pw_d3d9_transform_shadow *s, uint32_t state)
{
    int slot = pw_d3d9_transform_slot(state);
    if (!s) return;
    /* The backend multiplies the live value even while recording. The float
     * product is not reproduced locally: the next GetTransform asks. */
    if (slot < 0) pw_d3d9_transform_invalidate(s);
    else put_bit(s->live.known, (unsigned)slot, 0);
}
int pw_d3d9_transform_lookup(const struct pw_d3d9_transform_shadow *s, uint32_t state, void *matrix)
{
    int slot = pw_d3d9_transform_slot(state);
    if (!s || !matrix || slot < 0 || s->recording == PW_D3D9_TRANSFORM_RECORDING_UNKNOWN ||
        !test_bit(s->live.known, (unsigned)slot)) return 0;
    memcpy(matrix, s->live.value[slot], 64);
    return 1;
}
void pw_d3d9_transform_observe(struct pw_d3d9_transform_shadow *s, uint32_t state, const void *matrix, uint32_t hr)
{
    int slot = pw_d3d9_transform_slot(state);
    if (!s || !matrix || hr || slot < 0 || s->recording == PW_D3D9_TRANSFORM_RECORDING_UNKNOWN) return;
    memcpy(s->live.value[slot], matrix, 64);
    put_bit(s->live.known, (unsigned)slot, 1);
}
void pw_d3d9_transform_begin(struct pw_d3d9_transform_shadow *s, uint32_t hr)
{
    if (!s) return;
    if (hr) { uncertain(s); return; }
    s->recording = PW_D3D9_TRANSFORM_RECORDING;
    forget_pending(s);
}
void pw_d3d9_transform_block_prepare(struct pw_d3d9_transform_block *b)
{
    if (b && !b->set) b->set = malloc(sizeof(*b->set));
    if (b) b->known = 0;
}
void pw_d3d9_transform_block_free(struct pw_d3d9_transform_block *b)
{
    if (!b) return;
    free(b->set);
    b->set = NULL; b->known = 0;
}
void pw_d3d9_transform_end(struct pw_d3d9_transform_shadow *s, struct pw_d3d9_transform_block *b, uint32_t hr)
{
    if (!s) return;
    if (hr) { uncertain(s); return; }
    if (b) {
        b->known = s->recording == PW_D3D9_TRANSFORM_RECORDING && !s->pending_unknown && b->set;
        if (b->known) memcpy(b->set, &s->pending, sizeof(s->pending));
    }
    s->recording = PW_D3D9_TRANSFORM_LIVE;
    forget_pending(s);
}
void pw_d3d9_transform_create(struct pw_d3d9_transform_shadow *s, struct pw_d3d9_transform_block *b, uint32_t type, uint32_t hr)
{
    if (!s || hr) return;
    if (b) b->known = 0;
    /* The backend refuses creation while recording; success elsewhere means the
     * model lost track, so it does not prove anything. */
    if (s->recording != PW_D3D9_TRANSFORM_LIVE) { uncertain(s); return; }
    if (!b) return;
    if (type == 1 && b->set) {             /* D3DSBT_ALL captures every transform */
        memcpy(b->set->value, s->live.value, sizeof(b->set->value));
        memcpy(b->set->known, s->live.known, sizeof(b->set->known));
        memset(b->set->captured, 0xff, sizeof(b->set->captured));
        b->set->captured[PW_D3D9_TRANSFORM_WORDS - 1] = (1u << (PW_D3D9_TRANSFORM_SLOTS % 32)) - 1;
        b->known = 1;
    } else if (type == 2 || type == 3) {   /* pixel and vertex blocks capture no transform */
        if (b->set) { memset(b->set->captured, 0, sizeof(b->set->captured)); memset(b->set->known, 0, sizeof(b->set->known)); }
        b->known = 1;
    }
}
void pw_d3d9_transform_capture(struct pw_d3d9_transform_shadow *s, struct pw_d3d9_transform_block *b, uint32_t hr)
{
    if (!s) return;
    /* The backend refuses Capture only while recording, before any effect. */
    if (hr == REFUSED) { if (s->recording == PW_D3D9_TRANSFORM_LIVE) uncertain(s); return; }
    if (!hr && s->recording != PW_D3D9_TRANSFORM_LIVE) uncertain(s); /* only Begin/End re-establish it */
    if (!b || !b->known || !b->set) return;
    for (unsigned n = 0; n < PW_D3D9_TRANSFORM_SLOTS; n++) {
        if (!test_bit(b->set->captured, n)) continue;
        /* A failed call may still have captured: forget rather than guess. */
        int known = !hr && test_bit(s->live.known, n);
        put_bit(b->set->known, n, known);
        if (known) memcpy(b->set->value[n], s->live.value[n], 64);
    }
}
void pw_d3d9_transform_apply(struct pw_d3d9_transform_shadow *s, const struct pw_d3d9_transform_block *b, uint32_t hr)
{
    if (!s) return;
    if (hr == REFUSED) { if (s->recording == PW_D3D9_TRANSFORM_LIVE) uncertain(s); return; }
    if (!hr && s->recording != PW_D3D9_TRANSFORM_LIVE) { uncertain(s); return; }
    if (hr || !b || !b->known) { pw_d3d9_transform_invalidate(s); return; }
    if (!b->set) return;
    for (unsigned n = 0; n < PW_D3D9_TRANSFORM_SLOTS; n++) {
        if (!test_bit(b->set->captured, n)) continue;
        int known = test_bit(b->set->known, n);
        put_bit(s->live.known, n, known);
        if (known) memcpy(s->live.value[n], b->set->value[n], 64);
    }
}
