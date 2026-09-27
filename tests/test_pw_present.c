/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* GDI target -> PwPresentFrame -> fixed-extent sink.  The sink copies the
 * lent target into its own "scanout" during submit, as a display backend
 * must, so the test can observe ownership after the call returns. */
#include "../src/pw_present.h"
#include <assert.h>
#include <string.h>

enum { OUT_W=16,OUT_H=10,OUT_STRIDE=OUT_W*4+8,OUT_BYTES=OUT_STRIDE*OUT_H };

typedef struct TestSink {
    uint8_t target[OUT_BYTES],scanout[OUT_BYTES];
    uint32_t width,height;
    unsigned acquires,commits,abandons;
    uint64_t last_sequence;
    int acquire_status,submit_status;
} TestSink;

static int sink_acquire(void *context,PwPresentTarget *target)
{
    TestSink *sink=context;sink->acquires++;
    if(sink->acquire_status!=PW_OK)return sink->acquire_status;
    memset(sink->target,0xa5,sizeof(sink->target));
    *target=(PwPresentTarget){sink->target,sink->width,sink->height,OUT_STRIDE,OUT_BYTES};
    return PW_OK;
}
static int sink_submit(void *context,uint64_t sequence,int commit)
{
    TestSink *sink=context;
    if(!commit){sink->abandons++;return PW_OK;}
    sink->commits++;sink->last_sequence=sequence;
    if(sink->submit_status!=PW_OK)return sink->submit_status;
    memcpy(sink->scanout,sink->target,sizeof(sink->scanout));
    return PW_OK;
}
static const uint8_t *out_pixel(const TestSink *sink,uint32_t x,uint32_t y)
{
    return sink->scanout+(size_t)y*OUT_STRIDE+(size_t)x*4u;
}
static void expect_pixel(const TestSink *sink,uint32_t x,uint32_t y,
                         uint8_t b,uint8_t g,uint8_t r)
{
    const uint8_t *pixel=out_pixel(sink,x,y);
    assert(pixel[0]==b && pixel[1]==g && pixel[2]==r && pixel[3]==0xff);
}

static uint8_t scaled[32*32*4];
static const uint8_t *at_scaled(const PwPresentTarget *t,uint32_t x,uint32_t y)
{
    return t->pixels+(uint64_t)y*t->stride+(uint64_t)x*4u;
}
/* Frame pixel (x,y) is B=x, G=y, R=0x80. */
static void test_scale(void)
{
    uint8_t source[4*3*4];
    for(uint32_t y=0;y<3;y++)for(uint32_t x=0;x<4;x++) {
        uint8_t *p=source+(y*4+x)*4;p[0]=(uint8_t)x;p[1]=(uint8_t)y;p[2]=0x80;p[3]=0;
    }
    const PwPresentFrame frame={source,4,3,16,PW_PRESENT_BGRX8};
    PwPresentTarget wide={scaled,16,9,16*4,sizeof(scaled)},tall={scaled,9,16,9*4,sizeof(scaled)};
    PwPresentPlacement shown;
    const uint8_t *px;

    /* FIT keeps 4:3 and fills the height of 16:9: 12x9 at x=2, a whole 3x. */
    assert(pw_present_scale(&frame,PW_PRESENT_SCALE_FIT,0x102030u,&wide,&shown)==PW_OK);
    assert(shown.left==2 && shown.top==0 && shown.shown_width==12 && shown.shown_height==9 &&
           shown.scale==3);
    px=at_scaled(&wide,0,0);assert(px[0]==0x30 && px[1]==0x20 && px[2]==0x10 && px[3]==0xff);
    px=at_scaled(&wide,15,8);assert(px[0]==0x30);
    px=at_scaled(&wide,2,0);assert(px[0]==0 && px[1]==0 && px[2]==0x80 && px[3]==0xff);
    px=at_scaled(&wide,13,8);assert(px[0]==3 && px[1]==2);
    px=at_scaled(&wide,7,4);assert(px[0]==1 && px[1]==1);
    /* ... and the width of a tall target, letterboxed. */
    assert(pw_present_scale(&frame,PW_PRESENT_SCALE_FIT,0,&tall,&shown)==PW_OK);
    assert(shown.left==0 && shown.top==5 && shown.shown_width==9 && shown.shown_height==6 &&
           !shown.scale);
    px=at_scaled(&tall,8,10);assert(px[0]==3 && px[1]==2);
    px=at_scaled(&tall,0,4);assert(!px[0] && !px[1] && !px[2]);

    /* STRETCH fills the target; not a whole multiple here. */
    assert(pw_present_scale(&frame,PW_PRESENT_SCALE_STRETCH,0,&wide,&shown)==PW_OK);
    assert(!shown.left && !shown.top && shown.shown_width==16 && shown.shown_height==9 &&
           !shown.scale);
    px=at_scaled(&wide,0,0);assert(px[0]==0 && px[2]==0x80);
    px=at_scaled(&wide,15,8);assert(px[0]==3 && px[1]==2);
    assert(pw_present_scale(&frame,PW_PRESENT_SCALE_STRETCH,0,&wide,NULL)==PW_OK);

    /* INTEGER uses the whole-multiple placement and refuses larger frames. */
    assert(pw_present_scale(&frame,PW_PRESENT_SCALE_INTEGER,0,&wide,&shown)==PW_OK);
    assert(shown.scale==3 && shown.left==2 && shown.shown_width==12);
    PwPresentTarget small={scaled,3,3,3*4,sizeof(scaled)};
    assert(pw_present_scale(&frame,PW_PRESENT_SCALE_INTEGER,0,&small,&shown)==PW_ERR_LIMIT);
    assert(pw_present_scale(&frame,PW_PRESENT_SCALE_FIT,0,&small,&shown)==PW_OK &&
           shown.shown_width==3 && shown.shown_height==2);

    /* A one-pixel-high strip still shows at least one row. */
    uint8_t strip[20*4]={0};
    const PwPresentFrame line={strip,20,1,80,PW_PRESENT_BGRX8};
    PwPresentTarget square={scaled,2,2,8,sizeof(scaled)};
    assert(pw_present_scale(&line,PW_PRESENT_SCALE_FIT,0,&square,&shown)==PW_OK &&
           shown.shown_height==1 && shown.shown_width==2);

    /* The stepped columns equal floor(i*width/shown) for awkward sizes. */
    {
        static uint8_t odd[7*5*4];
        for(uint32_t i=0;i<sizeof(odd);i++)odd[i]=(uint8_t)(i*37u+11u);
        const PwPresentFrame seven={odd,7,5,28,PW_PRESENT_BGRX8};
        PwPresentTarget t={scaled,23,13,23*4,sizeof(scaled)};
        for(int mode=PW_PRESENT_SCALE_FIT;mode<=PW_PRESENT_SCALE_STRETCH;mode+=2) {
            assert(pw_present_scale(&seven,mode,0,&t,&shown)==PW_OK);
            for(uint32_t y=shown.top;y<shown.top+shown.shown_height;y++)
                for(uint32_t x=shown.left;x<shown.left+shown.shown_width;x++) {
                    uint32_t sx=(x-shown.left)*7u/shown.shown_width,sy=(y-shown.top)*5u/shown.shown_height;
                    const uint8_t *want=odd+(sy*7u+sx)*4u,*got=at_scaled(&t,x,y);
                    assert(got[0]==want[0] && got[1]==want[1] && got[2]==want[2] && got[3]==0xff);
                }
        }
    }
    /* The placement alone is the rectangle pw_present_scale reports. */
    {
        const PwPresentTarget *targets[]={&wide,&tall,&small,&square};
        const PwPresentFrame *frames[]={&frame,&line};
        PwPresentPlacement alone;
        for(int mode=PW_PRESENT_SCALE_FIT;mode<=PW_PRESENT_SCALE_STRETCH;mode++)
            for(size_t f=0;f<2;f++)for(size_t t=0;t<4;t++) {
                int drawn=pw_present_scale(frames[f],mode,0,targets[t],&shown);
                assert(pw_present_scale_placement(frames[f],mode,targets[t]->width,
                                                  targets[t]->height,&alone)==drawn);
                if(drawn==PW_OK)assert(!memcmp(&alone,&shown,sizeof(shown)));
            }
        assert(pw_present_scale_placement(&frame,PW_PRESENT_SCALE_FIT,1920,1080,&alone)==PW_OK);
        assert(alone.left==240 && !alone.top && alone.shown_width==1440 &&
               alone.shown_height==1080 && alone.scale==360);
        assert(pw_present_scale_placement(&frame,PW_PRESENT_SCALE_INTEGER,3,3,&alone)==PW_ERR_LIMIT);
        assert(pw_present_scale_placement(&frame,7,16,9,&alone)==PW_ERR_UNSUPPORTED);
        assert(pw_present_scale_placement(&frame,PW_PRESENT_SCALE_FIT,16,9,NULL)==PW_ERR_PRECONDITION);
        assert(pw_present_scale_placement(&frame,PW_PRESENT_SCALE_FIT,0,9,&alone)==PW_ERR_PRECONDITION);
        assert(pw_present_scale_placement(&frame,PW_PRESENT_SCALE_FIT,16,0,&alone)==PW_ERR_PRECONDITION);
        assert(pw_present_scale_placement(NULL,PW_PRESENT_SCALE_FIT,16,9,&alone)==PW_ERR_PRECONDITION);
    }
    assert(pw_present_scale(&frame,7,0,&wide,&shown)==PW_ERR_UNSUPPORTED);
    assert(pw_present_scale(&frame,PW_PRESENT_SCALE_FIT,0,NULL,&shown)==PW_ERR_PRECONDITION);
    PwPresentTarget bad=wide;bad.stride=10;
    assert(pw_present_scale(&frame,PW_PRESENT_SCALE_FIT,0,&bad,&shown)==PW_ERR_MALFORMED);
    bad=wide;bad.bytes=10;
    assert(pw_present_scale(&frame,PW_PRESENT_SCALE_FIT,0,&bad,&shown)==PW_ERR_MALFORMED);
    PwPresentFrame broken=frame;broken.format=0;
    assert(pw_present_scale(&broken,PW_PRESENT_SCALE_FIT,0,&wide,&shown)==PW_ERR_UNSUPPORTED);
}

int main(void)
{
    test_scale();
    /* A top-down image: blue encodes the row from the top (12 down to 10),
     * green the column, and alpha is left zero so the test proves the
     * presenter makes scanout opaque. */
    uint8_t pixels[4*3*4]={0};
    for(unsigned row=0;row<3;row++)for(unsigned x=0;x<4;x++) {
        uint8_t *pixel=pixels+(row*4+x)*4;
        pixel[0]=(uint8_t)(12-row);pixel[1]=(uint8_t)(20+x);pixel[2]=7;pixel[3]=0;
    }
    PwPresentView view={pixels,4,3,16,sizeof(pixels)};PwPresentFrame frame;
    assert(pw_present_frame_from_view(&view,&frame)==PW_OK);
    assert(frame.width==4 && frame.height==3 && frame.stride==16 &&
           frame.format==PW_PRESENT_BGRX8 && frame.pixels==view.pixels);

    TestSink sink;memset(&sink,0,sizeof(sink));sink.width=OUT_W;sink.height=OUT_H;
    const PwPresentSink backend={&sink,OUT_W,OUT_H,sink_acquire,sink_submit};
    PwPresentPlacement placement;memset(&placement,0,sizeof(placement));
    /* 16/4=4 and 10/3=3, clamped to max_scale 2: an 8x6 image at (4,2). */
    assert(pw_present_frame(&backend,&frame,0,2,0x00302010u,41,&placement)==PW_OK);
    assert(placement.scale==2 && placement.left==4 && placement.top==2 &&
           placement.shown_width==8 && placement.shown_height==6);
    assert(sink.acquires==1 && sink.commits==1 && !sink.abandons && sink.last_sequence==41);

    expect_pixel(&sink,0,0,0x10,0x20,0x30);
    expect_pixel(&sink,3,1,0x10,0x20,0x30);
    expect_pixel(&sink,12,2,0x10,0x20,0x30);
    expect_pixel(&sink,15,9,0x10,0x20,0x30);
    /* Upright: the first shown scanline is the image's top row. */
    for(uint32_t y=0;y<6;y++)for(uint32_t x=0;x<8;x++)
        expect_pixel(&sink,4+x,2+y,(uint8_t)(12-y/2),(uint8_t)(20+x/2),7);
    /* The padded bytes of each target row belong to the backend. */
    for(uint32_t y=0;y<OUT_H;y++)
        for(uint32_t pad=OUT_W*4;pad<OUT_STRIDE;pad++)
            assert(sink.scanout[(size_t)y*OUT_STRIDE+pad]==0xa5);

    /* After submit returns, the producer may draw again without changing
     * what the backend already copied out. */
    memset(pixels,0,sizeof(pixels));
    expect_pixel(&sink,4,2,12,20,7);
    assert(pw_present_frame_from_view(&view,&frame)==PW_OK);
    assert(pw_present_frame(&backend,&frame,0,1,0,42,&placement)==PW_OK);
    assert(placement.scale==1 && placement.left==6 && placement.top==3);
    expect_pixel(&sink,6,3,0,0,0);expect_pixel(&sink,0,0,0,0,0);
    assert(sink.commits==2 && sink.last_sequence==42);

    /* Margins shrink the usable area (12x6 -> scale 2) but never below one. */
    PwPresentFrame wide={view.pixels,4,3,16,PW_PRESENT_BGRX8};
    assert(pw_present_fit(&wide,16,10,4,3,&placement)==PW_OK &&
           placement.scale==2 && placement.left==4 && placement.top==2);
    assert(pw_present_fit(&wide,4,3,80,3,&placement)==PW_OK &&
           placement.scale==1 && !placement.left && !placement.top);

    /* Refusals happen before a target is acquired. */
    PwPresentFrame bad=frame;bad.stride=12;
    assert(pw_present_frame(&backend,&bad,0,1,0,1,&placement)==PW_ERR_MALFORMED);
    bad=frame;bad.stride=18;
    assert(pw_present_validate(&bad)==PW_ERR_MALFORMED);
    bad=frame;bad.format=2;
    assert(pw_present_validate(&bad)==PW_ERR_UNSUPPORTED);
    bad=frame;bad.pixels=NULL;
    assert(pw_present_validate(&bad)==PW_ERR_PRECONDITION);
    bad=frame;bad.width=OUT_W+1;bad.stride=(OUT_W+1)*4;
    assert(pw_present_frame(&backend,&bad,0,1,0,1,&placement)==PW_ERR_LIMIT);
    assert(sink.acquires==2);
    PwPresentView truncated=view;truncated.bytes=view.bytes-1;
    assert(pw_present_frame_from_view(&truncated,&frame)==PW_ERR_TRUNCATED);
    assert(pw_present_frame_from_view(NULL,&frame)==PW_ERR_PRECONDITION);
    assert(pw_present_fit(&wide,16,10,0,0,&placement)==PW_ERR_PRECONDITION);

    /* An acquire failure is returned without a submit. */
    assert(pw_present_frame_from_view(&view,&frame)==PW_OK);
    sink.acquire_status=PW_ERR_VM;
    assert(pw_present_frame(&backend,&frame,0,1,0,43,&placement)==PW_ERR_VM);
    assert(sink.acquires==3 && sink.commits==2 && !sink.abandons);
    sink.acquire_status=PW_OK;

    /* A target that does not match the declared extent is abandoned. */
    sink.width=OUT_W-1;
    assert(pw_present_frame(&backend,&frame,0,1,0,44,&placement)==PW_ERR_STATE);
    assert(sink.abandons==1 && sink.commits==2);
    sink.width=OUT_W;

    /* A failed flip is reported and does not publish a placement. */
    sink.submit_status=PW_ERR_LIMIT;memset(&placement,0,sizeof(placement));
    assert(pw_present_frame(&backend,&frame,0,1,0,45,&placement)==PW_ERR_LIMIT);
    assert(sink.commits==3 && !placement.scale);
    sink.submit_status=PW_OK;

    /* compose rejects placements that do not fit its target. */
    PwPresentTarget small={sink.target,4,3,16,48};
    PwPresentPlacement outside={1,0,1,4,3};
    assert(pw_present_compose(&frame,&outside,0,&small)==PW_ERR_LIMIT);
    PwPresentPlacement inconsistent={0,0,2,4,3};
    assert(pw_present_compose(&frame,&inconsistent,0,&small)==PW_ERR_LIMIT);
    PwPresentTarget short_target={sink.target,4,3,16,47};
    PwPresentPlacement exact={0,0,1,4,3};
    assert(pw_present_compose(&frame,&exact,0,&short_target)==PW_ERR_MALFORMED);
    assert(pw_present_compose(&frame,&exact,0,&small)==PW_OK);

    return 0;
}
