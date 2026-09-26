/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_WINE_DISPLAY_H
#define PW_WINE_DISPLAY_H
/*
 * The title's side of Wine's PS5 user driver (wine/ps5/pw_wine_sink.h).
 *
 * win32u hands each frame to the present sink on one of Wine's threads. The
 * sink only copies it into a PwWineFrameBox; the title's own thread takes the
 * newest frame and shows it, so the display backend never runs on a Wine
 * thread and a slow flip never stalls Wine. The same thread reads the pad and
 * turns its button edges into the key events Wine's driver drains.
 */
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include "../src/pw_gdi.h"
#include "../src/pw_pad.h"
#include "../wine/ps5/pw_wine_sink.h"

typedef struct PwWineFrameBox {
    pthread_mutex_t lock;
    uint8_t *pixels;              /* the caller's storage; rows packed, width * 4 bytes */
    size_t capacity;
    uint32_t width, height;
    uint64_t sequence;            /* frames put */
    uint64_t rejected;            /* too large, or invalid */
} PwWineFrameBox;

/* A box keeping frames of at most bytes (width * height * 4) in storage,
 * which the caller owns: a title's libc heap is too small for frames, so it
 * lends static memory. 0, or -1. */
int pw_wine_frame_box_init(PwWineFrameBox *box, uint8_t *storage, size_t bytes);
void pw_wine_frame_box_destroy(PwWineFrameBox *box);
/* Keep a copy of a BGRA frame, top-down, stride in bytes. 0, or -1 when it
 * is invalid or larger than the box. */
int pw_wine_frame_box_put(PwWineFrameBox *box, const void *bgra, uint32_t width,
                          uint32_t height, uint32_t stride);
/* When a frame newer than *seen was put, copy it into out (at most
 * capacity bytes), describe it in view, update *seen and return 1; 0 when
 * there is nothing new, -1 when out is too small. */
int pw_wine_frame_box_take(PwWineFrameBox *box, uint64_t *seen, uint8_t *out,
                           size_t capacity, PwGdiTargetView *view);

/* Key events for the pad's last batch: a release for each mapped button in
 * released_edges, then a press for each in pressed_edges, with the map's
 * virtual key. Returns how many were written (at most max). */
size_t pw_wine_pad_inputs(const PwPad *pad, PwWineInput *out, size_t max);
#endif
