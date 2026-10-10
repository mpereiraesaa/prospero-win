/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_VK_SPSC_H
#define PW_VK_SPSC_H
#include <stdatomic.h>
#include "pw_vk_command_stream.h"

/* One producer, one consumer per ring. Storage and nodes stay alive until both
 * have stopped. The adapter owns registry publication, retirement and drain
 * serialization; neither append nor take acquires an adapter lock. */
struct pw_vk_spsc_sequence { _Atomic uint64_t next; };
struct pw_vk_spsc {
    _Atomic uint64_t read, write;
    unsigned char *arena;
    size_t capacity;
};
void pw_vk_spsc_sequence_init(struct pw_vk_spsc_sequence *);
int pw_vk_spsc_init(struct pw_vk_spsc *, void *, size_t);
/* Highest reserved sequence, including any not yet published. A drain must
 * consume every sequence through this marker; later publications may proceed. */
uint64_t pw_vk_spsc_marker(const struct pw_vk_spsc_sequence *);
int pw_vk_spsc_append(struct pw_vk_spsc_sequence *, struct pw_vk_spsc *,
                     uint32_t opcode, const void *payload, uint32_t bytes);
/* Copy exactly the expected sequence into consumer-owned scratch, then release
 * its ring capacity. PENDING means empty or an earlier sequence belongs to a
 * different/unpublished producer. It never means that sequence may be skipped.
 * Output is existing stream framing, ready for full semantic batch preflight.
 * FULL and all errors leave the ring untouched. */
int pw_vk_spsc_take(struct pw_vk_spsc *, uint64_t expected, void *scratch,
                   size_t capacity, size_t *bytes);
#endif
