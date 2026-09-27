/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_VIDEOOUT_PS5_H
#define PW_VIDEOOUT_PS5_H
#include "../src/pw_present.h"
#include "pw_agc_ps5.h"
#include <stdint.h>
typedef struct PwVideoOutPs5 {
    void *memory;int64_t physical;size_t bytes,frame_bytes;
    PwAgcPs5 agc;
    int handle;unsigned index,opened,allocated,mapped,buffers_registered;uint64_t flips;
    int unregister_rc,close_rc,munmap_rc,release_rc,agc_close_rc;
} PwVideoOutPs5;
int pw_videoout_ps5_open(PwVideoOutPs5 *);
int pw_videoout_ps5_close(PwVideoOutPs5 *);
int pw_videoout_ps5_present(PwVideoOutPs5 *,const PwPresentView *);
/* frame scaled to the whole screen by mode (PW_PRESENT_SCALE_*), the rest
 * background (B,G,R), drawn straight into the tiled scanout: one pass over
 * the screen, where pw_present_scale then pw_videoout_ps5_present make two. */
int pw_videoout_ps5_present_scaled(PwVideoOutPs5 *,const PwPresentFrame *frame,int mode,
                                   uint32_t background);
#endif
