/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_PAD_H
#define PW_PAD_H
#include "pw_user32.h"
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
    uint64_t batches,samples,events,presses,releases,neutralizations;
    uint32_t max_batch;
} PwPadStats;

typedef struct PwPad {
    const PwPadKeyMap *map;
    size_t map_count;
    uint32_t previous_buttons;
    /* Raw edges observed in the most recent process batch.  These include
     * buttons with no Win32-key binding so a title adapter can implement
     * lifecycle actions (for example an orderly WM_QUIT) without teaching
     * the reusable mapper title-specific policy. */
    uint32_t pressed_edges,released_edges;
    uint8_t generation;
    unsigned generation_valid,connected;
    PwPadStats stats;
} PwPad;

int pw_pad_init(PwPad *,const PwPadKeyMap *,size_t);
int pw_pad_process(PwPad *,PwUser32 *,uint32_t window,
                   const PwPadSample *,size_t);
int pw_pad_neutralize(PwPad *,PwUser32 *,uint32_t window,uint64_t timestamp_us);
/* Consumes samples for a native UI with no Win32 destination: updates the
 * held buttons and the raw edges only.  Disconnection, interception and a
 * generation change release every button, exactly as process does, so a
 * later process call on the same pad never sees a phantom press. */
int pw_pad_track(PwPad *,const PwPadSample *,size_t);

#endif
