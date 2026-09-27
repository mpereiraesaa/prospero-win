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

/* A top-down 32-bit B,G,R,X image and the bytes behind it, as a producer
 * (Wine's PS5 user driver, the launcher) hands it over. */
typedef struct PwPresentView {
    const uint8_t *pixels;
    uint32_t width,height,stride,bytes;
} PwPresentView;

/* The frame for a view: PW_ERR_TRUNCATED when its rows do not fit its bytes. */
int pw_present_frame_from_view(const PwPresentView *view,PwPresentFrame *frame);
int pw_present_validate(const PwPresentFrame *frame);
int pw_present_fit(const PwPresentFrame *frame,uint32_t output_width,uint32_t output_height,
                   uint32_t margin,uint32_t max_scale,PwPresentPlacement *placement);
int pw_present_compose(const PwPresentFrame *frame,const PwPresentPlacement *placement,
                       uint32_t background,const PwPresentTarget *target);
/* How a profile shows its frames on the whole target. */
enum { PW_PRESENT_SCALE_FIT=0,PW_PRESENT_SCALE_INTEGER,PW_PRESENT_SCALE_STRETCH };
/* Nearest-neighbour scaling of frame onto all of target, the rest painted
 * background. FIT keeps the aspect ratio and fills one axis; INTEGER uses
 * the largest whole scale that fits (a frame larger than the target is
 * refused); STRETCH fills the target. placement, when not NULL, receives
 * the shown rectangle, with scale 0 when it is not a whole multiple. */
int pw_present_scale(const PwPresentFrame *frame,int mode,uint32_t background,
                     const PwPresentTarget *target,PwPresentPlacement *placement);
/* The rectangle pw_present_scale shows frame in on a target of that size,
 * without drawing, for a backend that scales into its own layout. Source
 * row and column i of the shown rectangle are floor(i*height/shown_height)
 * and floor(i*width/shown_width). */
int pw_present_scale_placement(const PwPresentFrame *frame,int mode,uint32_t target_width,
                               uint32_t target_height,PwPresentPlacement *placement);
/* validate -> fit -> acquire -> compose -> submit.  Every refusal happens
 * before acquire, so a malformed or oversized frame never holds an image. */
int pw_present_frame(const PwPresentSink *sink,const PwPresentFrame *frame,
                     uint32_t margin,uint32_t max_scale,uint32_t background,
                     uint64_t sequence,PwPresentPlacement *placement);

#endif
