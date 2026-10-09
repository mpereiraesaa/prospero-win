/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_TRANSFORM_SHADOW_H
#define PW_D3D9_TRANSFORM_SHADOW_H
#include <stdint.h>
/* Proxy-side copy of the device transforms for pinned DXVK 5fde742b, so a
 * GetTransform can be answered without a round trip. Every entry starts
 * unknown and becomes known only from an exact S_OK outcome; anything the
 * model cannot follow makes entries unknown, which means "ask the service".
 * The caller serializes every update with the matching wire call. */
#define PW_D3D9_TRANSFORM_SLOTS 266u
#define PW_D3D9_TRANSFORM_WORDS ((PW_D3D9_TRANSFORM_SLOTS + 31u) / 32u)
struct pw_d3d9_transform_set {
    uint32_t captured[PW_D3D9_TRANSFORM_WORDS], known[PW_D3D9_TRANSFORM_WORDS];
    unsigned char value[PW_D3D9_TRANSFORM_SLOTS][64];
};
/* Evidence owned by one stateblock proxy shell. known=0: applying the block
 * makes every live transform unknown. set=NULL with known=1: the block
 * captures no transform (pixel/vertex blocks). */
struct pw_d3d9_transform_block {
    int known;
    struct pw_d3d9_transform_set *set;
};
enum pw_d3d9_transform_recording {
    PW_D3D9_TRANSFORM_LIVE, PW_D3D9_TRANSFORM_RECORDING, PW_D3D9_TRANSFORM_RECORDING_UNKNOWN
};
struct pw_d3d9_transform_shadow {
    enum pw_d3d9_transform_recording recording;
    int pending_unknown;
    struct pw_d3d9_transform_set live, pending;
};
/* State number to backend slot, or -1 for states the shadow never answers. */
int pw_d3d9_transform_slot(uint32_t state);
/* A freshly created device is not recording; its values are still unknown. */
void pw_d3d9_transform_init(struct pw_d3d9_transform_shadow *);
void pw_d3d9_transform_invalidate(struct pw_d3d9_transform_shadow *);
/* matrix NULL means identity, as the backend converts it. */
void pw_d3d9_transform_set(struct pw_d3d9_transform_shadow *, uint32_t state, const void *matrix, uint32_t hr);
void pw_d3d9_transform_multiply(struct pw_d3d9_transform_shadow *, uint32_t state);
/* Returns 1 and copies 64 bytes when the live value is known. */
int pw_d3d9_transform_lookup(const struct pw_d3d9_transform_shadow *, uint32_t state, void *matrix);
void pw_d3d9_transform_observe(struct pw_d3d9_transform_shadow *, uint32_t state, const void *matrix, uint32_t hr);
void pw_d3d9_transform_begin(struct pw_d3d9_transform_shadow *, uint32_t hr);
/* Allocate a block's snapshot storage before the Create/End RPC, outside any
 * serialization gate. Failure leaves the block unknown; it is not an error. */
void pw_d3d9_transform_block_prepare(struct pw_d3d9_transform_block *);
/* Final local retirement of the stateblock shell, never a wire RELEASE. */
void pw_d3d9_transform_block_free(struct pw_d3d9_transform_block *);
/* Outcome observers. A NULL block is an unknown block. */
void pw_d3d9_transform_end(struct pw_d3d9_transform_shadow *, struct pw_d3d9_transform_block *, uint32_t hr);
void pw_d3d9_transform_create(struct pw_d3d9_transform_shadow *, struct pw_d3d9_transform_block *, uint32_t type, uint32_t hr);
void pw_d3d9_transform_capture(struct pw_d3d9_transform_shadow *, struct pw_d3d9_transform_block *, uint32_t hr);
void pw_d3d9_transform_apply(struct pw_d3d9_transform_shadow *, const struct pw_d3d9_transform_block *, uint32_t hr);
#endif
