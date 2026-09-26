/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../native/pw_videoout_layout.h"
#include <assert.h>

static void expect(uint32_t w,uint32_t h,uint32_t sw,uint32_t sh,
                   uint32_t left,uint32_t top,uint32_t scale)
{
    PwVideoOutLayout l;
    assert(pw_videoout_layout(w,h,1920,1080,80,3,&l)==PW_OK);
    assert(l.source_width==sw && l.source_height==sh && l.left==left &&
           l.top==top && l.scale==scale);
    /* Every written column and row stays inside the scanout. */
    assert((uint64_t)l.left+(uint64_t)l.source_width*l.scale<=1920);
    assert((uint64_t)l.top+(uint64_t)l.source_height*l.scale<=1080);
    assert(l.source_width<=w && l.source_height<=h);
}

int main(void)
{
    /* Pinball's hardware-validated placement is unchanged. */
    expect(640,480,640,480,320,60,2);
    expect(600,416,600,416,360,124,2);
    expect(1920,1080,1920,1080,0,0,1);
    /* Between the margin and the edge: scale 1, still centred. */
    expect(1900,1060,1900,1060,10,10,1);
    /* Larger than the scanout: the old code underflowed left/top here. */
    expect(2000,1000,1920,1000,0,40,1);
    expect(800,1200,800,1080,560,0,1);
    expect(4096,4096,1920,1080,0,0,1);
    expect(UINT32_MAX/4u,1,1920,1,0,539,1);
    /* A narrow tall target keeps scale 1 on both axes. */
    expect(1,2000,1,1080,959,0,1);
    /* Every target the previous formula placed inside the scanout keeps its
     * exact placement; only the out-of-bounds cases changed. */
    for(uint32_t w=1;w<=1920;w+=7)for(uint32_t h=1;h<=1080;h+=5) {
        uint32_t sx=1840u/w,sy=1000u/h,scale=sx<sy?sx:sy;
        if(!scale)scale=1;
        if(scale>3)scale=3;
        PwVideoOutLayout old;
        assert(pw_videoout_layout(w,h,1920,1080,80,3,&old)==PW_OK);
        assert(old.scale==scale && old.source_width==w && old.source_height==h &&
               old.left==(1920-w*scale)/2 && old.top==(1080-h*scale)/2);
    }
    PwVideoOutLayout l;
    assert(pw_videoout_layout(0,1,1920,1080,80,3,&l)==PW_ERR_PRECONDITION);
    assert(pw_videoout_layout(1,1,1920,1080,80,0,&l)==PW_ERR_PRECONDITION);
    assert(pw_videoout_layout(1,1,1920,1080,80,3,NULL)==PW_ERR_PRECONDITION);
    return 0;
}
