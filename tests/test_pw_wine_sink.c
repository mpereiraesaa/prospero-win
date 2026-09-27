/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../wine/ps5/pw_wine_sink.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <unistd.h>

static int calls,fail_next;
static uint32_t seen_w,seen_h,seen_stride,first_pixel;
static void *seen_context;

static int sink(void *context,const void *bgra,uint32_t w,uint32_t h,uint32_t stride)
{
    calls++;seen_context=context;seen_w=w;seen_h=h;seen_stride=stride;
    memcpy(&first_pixel,bgra,4);
    if(fail_next){fail_next=0;return -7;}
    return 0;
}

static void *poster(void *arg)
{
    (void)arg;
    for(int i=0;i<100;i++) {
        PwWineInput e={PW_WINE_INPUT_MOUSE_MOVE,0,i,i,0};
        while(pw_wine_post_input(&e))sched_yield();
    }
    return NULL;
}

int main(void)
{
    uint32_t frame[4*3];
    for(int i=0;i<12;i++)frame[i]=0xff000000u|(uint32_t)i;
    PwWineSinkStats s;

    /* No sink: dropped, not delivered anywhere. */
    assert(pw_wine_present(frame,4,3,16)==-1);
    pw_wine_sink_stats(&s);assert(s.frames==0 && s.frames_dropped==1);

    int context_tag;
    pw_wine_set_present_sink(sink,&context_tag);
    assert(pw_wine_present(frame,4,3,16)==0 && calls==1);
    assert(seen_context==&context_tag && seen_w==4 && seen_h==3 && seen_stride==16 && first_pixel==0xff000000u);
    /* A stride shorter than a row, empty sizes and NULL pixels are refused. */
    assert(pw_wine_present(frame,4,3,12)==-1 && pw_wine_present(frame,0,3,16)==-1);
    assert(pw_wine_present(NULL,4,3,16)==-1 && calls==1);
    /* The sink's failure is returned and counted. */
    fail_next=1;assert(pw_wine_present(frame,4,3,16)==-7 && calls==2);
    pw_wine_sink_stats(&s);assert(s.frames==1 && s.frames_dropped==5);

    /* Input is first in, first out, validated, and bounded. */
    PwWineInput e,out;
    assert(!pw_wine_next_input(&out) && !pw_wine_next_input(NULL));
    e=(PwWineInput){PW_WINE_INPUT_KEY,0x0d,0,0,1};assert(!pw_wine_post_input(&e));
    e=(PwWineInput){PW_WINE_INPUT_MOUSE_BUTTON,1,0,0,0};assert(!pw_wine_post_input(&e));
    assert(pw_wine_next_input(&out) && out.type==PW_WINE_INPUT_KEY && out.code==0x0d && out.down==1);
    assert(pw_wine_next_input(&out) && out.type==PW_WINE_INPUT_MOUSE_BUTTON && out.code==1 && !out.down);
    assert(!pw_wine_next_input(&out));
    PwWineInput bad[]={{0,1,0,0,1},{PW_WINE_INPUT_KEY,0,0,0,1},{PW_WINE_INPUT_KEY,256,0,0,1},
                       {PW_WINE_INPUT_KEY,65,0,0,2},{PW_WINE_INPUT_MOUSE_BUTTON,3,0,0,1},{99,0,0,0,0}};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++)assert(pw_wine_post_input(&bad[i])==-1);
    assert(pw_wine_post_input(NULL)==-1);
    e=(PwWineInput){PW_WINE_INPUT_MOUSE_MOVE,0,1,2,0};
    for(int i=0;i<PW_WINE_INPUT_QUEUE;i++)assert(!pw_wine_post_input(&e));
    assert(pw_wine_post_input(&e)==-1);
    for(int i=0;i<PW_WINE_INPUT_QUEUE;i++)assert(pw_wine_next_input(&out) && out.x==1 && out.y==2);
    assert(!pw_wine_next_input(&out));
    pw_wine_sink_stats(&s);
    assert(s.inputs_posted==2+PW_WINE_INPUT_QUEUE && s.inputs_delivered==s.inputs_posted);
    assert(s.inputs_dropped==sizeof(bad)/sizeof(bad[0])+2);

    /* The wake pipe: readable after a post, empty once drained. */
    int fd=pw_wine_input_fd();char drain[64];
    assert(fd>=0 && fd==pw_wine_input_fd());
    while(read(fd,drain,sizeof(drain))>0){}
    assert(read(fd,drain,1)==-1);
    e=(PwWineInput){PW_WINE_INPUT_KEY,0x41,0,0,1};assert(!pw_wine_post_input(&e));
    assert(read(fd,drain,sizeof(drain))==1 && pw_wine_next_input(&out) && out.code==0x41);
    assert(pw_wine_post_input(&bad[0])==-1 && read(fd,drain,1)==-1);  /* refused: no wake */

    /* A producer thread and this consumer: order kept, nothing lost. */
    pthread_t thread;assert(!pthread_create(&thread,NULL,poster,NULL));
    for(int expected=0;expected<100;) if(pw_wine_next_input(&out)) {assert(out.x==expected);expected++;}
    assert(!pthread_join(thread,NULL));

    /* The gamepad: none until the title sets one. */
    PwWinePad pad,got;
    memset(&got,0x5a,sizeof(got));
    assert(!pw_wine_pad(&got) && got.connected==0 && got.packet==0 && got.buttons==0);
    assert(!pw_wine_pad(NULL));
    pad=(PwWinePad){0,77,0x1000,10,255,-32768,32767,0,-1};
    pw_wine_set_pad(&pad);
    assert(pw_wine_pad(&got) && got.connected==1 && got.packet==1);  /* the sink numbers it */
    assert(got.buttons==0x1000 && got.left_trigger==10 && got.right_trigger==255);
    assert(got.thumb_lx==-32768 && got.thumb_ly==32767 && got.thumb_rx==0 && got.thumb_ry==-1);
    /* The same state keeps its packet; any change takes a new one. */
    pw_wine_set_pad(&pad);assert(pw_wine_pad(&got) && got.packet==1);
    PwWinePad changes[7];
    for(int i=0;i<7;i++)changes[i]=pad;
    changes[0].buttons=0x2000;changes[1].left_trigger=11;changes[2].right_trigger=254;
    changes[3].thumb_lx=1;changes[4].thumb_ly=2;changes[5].thumb_rx=3;changes[6].thumb_ry=4;
    for(uint32_t i=0;i<7;i++) {
        pw_wine_set_pad(&changes[i]);
        assert(pw_wine_pad(&got) && got.packet==2+i);
    }
    /* Disconnected: nothing to read; reconnecting is a new packet. */
    pw_wine_set_pad(NULL);
    assert(!pw_wine_pad(&got) && got.connected==0 && got.buttons==0);
    pw_wine_set_pad(&changes[6]);assert(pw_wine_pad(&got) && got.packet==9 && got.thumb_ry==4);

    /* Rumble: reported once per change, clamped to 16 bits. */
    uint32_t left=1,right=1;
    assert(!pw_wine_rumble(&left,&right) && left==0 && right==0);
    pw_wine_set_rumble(1000,70000);
    assert(pw_wine_rumble(&left,&right)==1 && left==1000 && right==0xffff);
    assert(!pw_wine_rumble(&left,&right) && left==1000 && right==0xffff);
    pw_wine_set_rumble(1000,0xffff);assert(!pw_wine_rumble(NULL,NULL));  /* unchanged */
    pw_wine_set_rumble(0x10000,0);assert(pw_wine_rumble(NULL,&right)==1 && right==0);
    assert(!pw_wine_rumble(&left,NULL) && left==0xffff);
    return 0;
}
