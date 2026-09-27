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
 * keys or mouse buttons, and a stick can move the pointer.
 */
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include "../src/pw_gdi.h"
#include "../src/pw_game_profile.h"
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

/* Wine input for one pad batch's edges under a profile's bindings: a
 * release for each bound button in released, then a press for each in
 * pressed, as a key or a mouse button. Returns how many were written (at
 * most max). */
size_t pw_wine_game_inputs(const PwGameInput *input, uint32_t pressed, uint32_t released,
                           PwWineInput *out, size_t max);

/* A pointer a stick moves over Wine's desktop, in 1/65536 pixels. */
typedef struct PwWinePointer {
    int64_t x, y;
    uint32_t width, height;
} PwWinePointer;

enum { PW_WINE_POINTER_DEADZONE = 20 };   /* of 128, stick noise at rest */

/* A pointer in the middle of a width x height desktop. */
void pw_wine_pointer_init(PwWinePointer *pointer, uint32_t width, uint32_t height);
/* The desktop the pointer moves over is now width x height (a frame of
 * another size): the pointer keeps its position, brought inside. With
 * WINE_PS5_VIEW=window, a menu that opens widens the frame from the same
 * top left, so the pointer stays on what it was over. */
void pw_wine_pointer_resize(PwWinePointer *pointer, uint32_t width, uint32_t height);
/* Move by a stick position (0..255 each axis, 0x80 centred) held for
 * elapsed_us, at up to speed pixels per second at full tilt, on a squared
 * curve past the dead zone, kept on the desktop. 1 and an absolute
 * MOUSE_MOVE in out when the pointer reached another pixel, else 0. */
int pw_wine_pointer_step(PwWinePointer *pointer, uint8_t stick_x, uint8_t stick_y,
                         uint32_t speed, uint32_t elapsed_us, PwWineInput *out);
#endif
