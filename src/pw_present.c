/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_present.h"
#include "pw_gdi.h"
#include <string.h>

int pw_present_frame_from_gdi(const PwGdiTargetView *view,PwPresentFrame *frame)
{
    if(!view || !frame)return PW_ERR_PRECONDITION;
    /* pw_gdi target surfaces are top-down 32-bit B,G,R,A rows; resource
     * DIBs are flipped when they are drawn, never when presented. */
    PwPresentFrame candidate={view->pixels,view->width,view->height,view->stride,
        PW_PRESENT_BGRX8};
    int status=pw_present_validate(&candidate);if(status!=PW_OK)return status;
    if((uint64_t)view->stride*view->height>view->bytes)return PW_ERR_TRUNCATED;
    *frame=candidate;return PW_OK;
}
int pw_present_validate(const PwPresentFrame *frame)
{
    if(!frame || !frame->pixels || !frame->width || !frame->height)return PW_ERR_PRECONDITION;
    if(frame->format!=PW_PRESENT_BGRX8)return PW_ERR_UNSUPPORTED;
    if(frame->width>UINT32_MAX/PW_PRESENT_BYTES_PER_PIXEL ||
       frame->stride<frame->width*PW_PRESENT_BYTES_PER_PIXEL ||
       frame->stride%PW_PRESENT_BYTES_PER_PIXEL)return PW_ERR_MALFORMED;
    return PW_OK;
}
int pw_present_fit(const PwPresentFrame *frame,uint32_t output_width,uint32_t output_height,
                   uint32_t margin,uint32_t max_scale,PwPresentPlacement *placement)
{
    if(!placement || !output_width || !output_height || !max_scale)return PW_ERR_PRECONDITION;
    int status=pw_present_validate(frame);if(status!=PW_OK)return status;
    /* A frame larger than the output is refused rather than cropped or
     * written out of bounds: the display route has one fixed extent. */
    if(frame->width>output_width || frame->height>output_height)return PW_ERR_LIMIT;
    uint32_t usable_w=output_width>margin?output_width-margin:0;
    uint32_t usable_h=output_height>margin?output_height-margin:0;
    uint32_t scale_x=usable_w/frame->width,scale_y=usable_h/frame->height;
    uint32_t scale=scale_x<scale_y?scale_x:scale_y;
    if(!scale)scale=1;
    if(scale>max_scale)scale=max_scale;
    uint32_t shown_w=frame->width*scale,shown_h=frame->height*scale;
    *placement=(PwPresentPlacement){(output_width-shown_w)/2u,(output_height-shown_h)/2u,
        scale,shown_w,shown_h};
    return PW_OK;
}
static void store_opaque(uint8_t *out,uint32_t bgrx)
{
    out[0]=(uint8_t)bgrx;out[1]=(uint8_t)(bgrx>>8);out[2]=(uint8_t)(bgrx>>16);out[3]=0xff;
}
int pw_present_compose(const PwPresentFrame *frame,const PwPresentPlacement *placement,
                       uint32_t background,const PwPresentTarget *target)
{
    if(!placement || !target || !target->pixels || !target->width || !target->height)
        return PW_ERR_PRECONDITION;
    int status=pw_present_validate(frame);if(status!=PW_OK)return status;
    if(target->width>UINT32_MAX/PW_PRESENT_BYTES_PER_PIXEL ||
       target->stride<target->width*PW_PRESENT_BYTES_PER_PIXEL ||
       target->stride%PW_PRESENT_BYTES_PER_PIXEL ||
       (uint64_t)target->stride*target->height>target->bytes)return PW_ERR_MALFORMED;
    if(!placement->scale ||
       placement->shown_width!=(uint64_t)frame->width*placement->scale ||
       placement->shown_height!=(uint64_t)frame->height*placement->scale ||
       placement->left>target->width || placement->shown_width>target->width-placement->left ||
       placement->top>target->height || placement->shown_height>target->height-placement->top)
        return PW_ERR_LIMIT;
    for(uint32_t y=0;y<target->height;y++) {
        uint8_t *row=target->pixels+(uint64_t)y*target->stride;
        for(uint32_t x=0;x<target->width;x++)store_opaque(row+(uint64_t)x*4u,background);
    }
    for(uint32_t y=0;y<placement->shown_height;y++) {
        const uint8_t *source=frame->pixels+(uint64_t)(y/placement->scale)*frame->stride;
        uint8_t *row=target->pixels+(uint64_t)(placement->top+y)*target->stride+
            (uint64_t)placement->left*4u;
        for(uint32_t x=0;x<placement->shown_width;x++) {
            const uint8_t *pixel=source+(uint64_t)(x/placement->scale)*4u;
            row[x*4u]=pixel[0];row[x*4u+1]=pixel[1];row[x*4u+2]=pixel[2];row[x*4u+3]=0xff;
        }
    }
    return PW_OK;
}
int pw_present_frame(const PwPresentSink *sink,const PwPresentFrame *frame,
                     uint32_t margin,uint32_t max_scale,uint32_t background,
                     uint64_t sequence,PwPresentPlacement *placement)
{
    if(!sink || !sink->acquire || !sink->submit || !placement)return PW_ERR_PRECONDITION;
    PwPresentPlacement fit;
    int status=pw_present_fit(frame,sink->width,sink->height,margin,max_scale,&fit);
    if(status!=PW_OK)return status;
    PwPresentTarget target;memset(&target,0,sizeof(target));
    if((status=sink->acquire(sink->context,&target))!=PW_OK)return status;
    if(target.width!=sink->width || target.height!=sink->height)status=PW_ERR_STATE;
    else status=pw_present_compose(frame,&fit,background,&target);
    if(status!=PW_OK) {
        (void)sink->submit(sink->context,sequence,0);return status;
    }
    if((status=sink->submit(sink->context,sequence,1))!=PW_OK)return status;
    *placement=fit;return PW_OK;
}
