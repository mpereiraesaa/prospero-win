/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_PAD_H
#define PW_PAD_H
#include "../include/prospero_win.h"
#include <stddef.h>
#include <stdint.h>

typedef struct PwPadSample {
    uint32_t buttons;
    uint64_t timestamp_us;
    uint8_t generation;
    unsigned connected,intercepted;
} PwPadSample;

typedef struct PwPadKeyMap {
    uint32_t mask;
    uint16_t virtual_key;
    uint8_t scan_code,extended;
    const char *action;
} PwPadKeyMap;

typedef struct PwPadStats {
    uint64_t batches,samples;
    uint32_t max_batch;
} PwPadStats;

typedef struct PwPad {
    const PwPadKeyMap *map;
    size_t map_count;
    uint32_t previous_buttons;
    /* Raw edges observed in the most recent batch, bound or not; the
     * title maps them (a game's bindings, the launcher's selection). */
    uint32_t pressed_edges,released_edges;
    uint8_t generation;
    unsigned generation_valid,connected;
    PwPadStats stats;
} PwPad;

int pw_pad_init(PwPad *,const PwPadKeyMap *,size_t);
/* Consumes samples: updates the held buttons and the raw edges.
 * Disconnection, interception and a generation change release every
 * button, so a later batch never sees a phantom press. */
int pw_pad_track(PwPad *,const PwPadSample *,size_t);

#endif
