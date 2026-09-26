/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_VIDEOOUT_LAYOUT_H
#define PW_VIDEOOUT_LAYOUT_H
#include "../include/prospero_win.h"
#include <stdint.h>

/* Where a GDI target lands on the fixed scanout.  The shown source
 * rectangle starts at the target's origin, is scaled by an integer factor
 * and never leaves the output: an axis that fits is centred, and an axis
 * larger than the output shows its leading part at scale 1, as a Windows
 * desktop shows a window that extends past its edge. */
typedef struct PwVideoOutLayout {
    uint32_t source_width,source_height;
    uint32_t left,top,scale;
} PwVideoOutLayout;

static inline int pw_videoout_layout(uint32_t width,uint32_t height,
                                     uint32_t output_width,uint32_t output_height,
                                     uint32_t margin,uint32_t max_scale,
                                     PwVideoOutLayout *layout)
{
    if(!layout || !width || !height || !output_width || !output_height || !max_scale)
        return PW_ERR_PRECONDITION;
    uint32_t usable_w=output_width>margin?output_width-margin:0;
    uint32_t usable_h=output_height>margin?output_height-margin:0;
    uint32_t scale_x=usable_w/width,scale_y=usable_h/height;
    uint32_t scale=scale_x<scale_y?scale_x:scale_y;
    if(!scale)scale=1;
    if(scale>max_scale)scale=max_scale;
    uint32_t shown_w=width<=output_width/scale?width:output_width/scale;
    uint32_t shown_h=height<=output_height/scale?height:output_height/scale;
    *layout=(PwVideoOutLayout){shown_w,shown_h,
        (output_width-shown_w*scale)/2u,(output_height-shown_h*scale)/2u,scale};
    return PW_OK;
}

#endif
