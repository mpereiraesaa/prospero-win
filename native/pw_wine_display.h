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
 * turns it into Wine input with the game profile's bindings: buttons send
 * keys or mouse buttons, and a stick can move the pointer. In xinput mode it
 * also becomes the game's XInput controller (the sink's gamepad slot).
 */
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include "../src/pw_present.h"
#include "../src/pw_game_profile.h"
#include "../wine/ps5/pw_wine_sink.h"
#include "pw_pad_ps5.h"

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
                           size_t capacity, PwPresentView *view);

/* Wine input for one pad batch's edges under a profile's bindings: a
 * release for each bound button in released, then a press for each in
 * pressed, as a key or a mouse button. Returns how many were written (at
 * most max). */
size_t pw_wine_game_inputs(const PwGameInput *input, uint32_t pressed, uint32_t released,
                           PwWineInput *out, size_t max);

/* The DualSense as an XInput gamepad: its held buttons (Cross A, Circle B,
 * Square X, Triangle Y, L1/R1 shoulders, L3/R3 thumbs, Options Start,
 * Create Back, the touchpad Guide), the analog triggers, and the sticks
 * scaled to -32768..32767 with y up. 1 and the state in out while the pad
 * is connected; 0 and a neutral state otherwise. The sink numbers the
 * packets. */
int pw_wine_game_pad(const PwPadPs5 *pad, PwWinePad *out);

/* A stick's pointer motion not yet sent, in 1/65536 pixels. Wine keeps
 * the pointer's position: the title sends only motion, as a mouse does
 * (patch 0670), so a game can move or clip the pointer itself and motion
 * against an edge still reaches it. */
typedef struct PwWinePointer {
    int64_t x, y;
} PwWinePointer;

enum { PW_WINE_POINTER_DEADZONE = 20 };   /* of 128, stick noise at rest */

/* Move by a stick position (0..255 each axis, 0x80 centred) held for
 * elapsed_us, at up to speed pixels per second at full tilt, on a squared
 * curve past the dead zone. 1 and a MOUSE_MOVE of the whole pixels moved
 * in out when there are any (the fraction is kept for the next step),
 * else 0. */
int pw_wine_pointer_step(PwWinePointer *pointer, uint8_t stick_x, uint8_t stick_y,
                         uint32_t speed, uint32_t elapsed_us, PwWineInput *out);
#endif
