/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_PRESENT_H
#define PW_PRESENT_H
#include "../include/prospero_win.h"

/* Backend-neutral presentation seam between a CPU-composed Windows surface
 * and a display backend.  Nothing here names Vulkan, AGC or VideoOut: the
 * direct-GDI bootstrap and a future Wine display driver produce the same
 * PwPresentFrame, and a native backend implements PwPresentSink. */

enum {
    /* Bytes are B,G,R,X per pixel, rows top-down.  X is ignored on input
     * and written as 0xff on output (opaque scanout). */
    PW_PRESENT_BGRX8=1,
    PW_PRESENT_BYTES_PER_PIXEL=4
};

/* A borrowed, CPU-visible frame.  The producer keeps ownership; the view is
 * valid only until the producer next resizes, destroys or resets the
 * surface, so it must be consumed synchronously on the producer's thread. */
typedef struct PwPresentFrame {
    const uint8_t *pixels;
    uint32_t width,height,stride;
    uint32_t format;
} PwPresentFrame;

/* Integer nearest-neighbour placement of a frame inside the output. */
typedef struct PwPresentPlacement {
    uint32_t left,top,scale,shown_width,shown_height;
} PwPresentPlacement;

/* Writable linear output lent by a backend between acquire and submit. */
typedef struct PwPresentTarget {
    uint8_t *pixels;
    uint32_t width,height,stride;
    uint64_t bytes;
} PwPresentTarget;

/* A backend with one fixed output extent lends one linear BGRX target per
 * frame.  After a successful acquire the caller calls submit exactly once:
 * commit=1 presents, commit=0 abandons the target without a flip.  submit is
 * synchronous: when a commit returns PW_OK the backend has copied the target
 * out and retired the flip, so the lent memory may be reused. */
typedef struct PwPresentSink {
    void *context;
    uint32_t width,height;
    int (*acquire)(void *context,PwPresentTarget *target);
    int (*submit)(void *context,uint64_t sequence,int commit);
} PwPresentSink;

struct PwGdiTargetView;

int pw_present_frame_from_gdi(const struct PwGdiTargetView *view,PwPresentFrame *frame);
int pw_present_validate(const PwPresentFrame *frame);
int pw_present_fit(const PwPresentFrame *frame,uint32_t output_width,uint32_t output_height,
                   uint32_t margin,uint32_t max_scale,PwPresentPlacement *placement);
int pw_present_compose(const PwPresentFrame *frame,const PwPresentPlacement *placement,
                       uint32_t background,const PwPresentTarget *target);
/* validate -> fit -> acquire -> compose -> submit.  Every refusal happens
 * before acquire, so a malformed or oversized frame never holds an image. */
int pw_present_frame(const PwPresentSink *sink,const PwPresentFrame *frame,
                     uint32_t margin,uint32_t max_scale,uint32_t background,
                     uint64_t sequence,PwPresentPlacement *placement);

#endif
