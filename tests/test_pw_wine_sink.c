/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _DEFAULT_SOURCE  /* usleep, CLOCK_THREAD_CPUTIME_ID */
#include "../wine/ps5/pw_wine_sink.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <time.h>
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

static int release_calls,release_status;
static void *release_seen;
static int release(void *context)
{
    release_calls++;release_seen=context;
    return release_status;
}

/* The title's callback as native/wine64_main.c has it: it asks the main
 * thread to close the video output and waits, a bounded time, until it has.
 * Its main thread keeps posting input and the pad's state meanwhile. */
static int title_request,title_closed,title_calls,title_main_done;
static int title_release(void *context)
{
    (void)context;
    __atomic_add_fetch(&title_calls,1,__ATOMIC_ACQ_REL);
    __atomic_store_n(&title_request,1,__ATOMIC_RELEASE);
    for(int waited=0;waited<2000;waited++) {
        if(__atomic_load_n(&title_closed,__ATOMIC_ACQUIRE))return 0;
        usleep(1000);
    }
    return -1;
}
static uint64_t thread_cpu_ns(void)
{
    struct timespec t;
    assert(!clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t));
    return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;
}
/* CPU time each thread spent while the release was pending: a thread that
 * spins on the sink (a busy-wait or a trylock loop) uses all of it. */
enum { TITLE_FRAMES=200, BUSY_LIMIT_NS=40000000 };
static uint64_t title_main_cpu_ns;
static void *title_main(void *arg)
{
    uint32_t frame[4*3]={0};
    PwWinePad pad={0},seen;
    PwWineInput e={PW_WINE_INPUT_KEY,0x20,0,0,1},out;
    uint32_t left,right;
    (void)arg;
    while(!__atomic_load_n(&title_request,__ATOMIC_ACQUIRE))sched_yield();
    /* The main loop's frames while the request waits (here 200 of them, a
     * millisecond apart): the pad, keys, rumble, a GDI frame from a Wine
     * thread and the game reading the pad. Each of these waited for the
     * release to give up before the lock was split; none may spin either. */
    const uint64_t cpu=thread_cpu_ns();
    for(int i=0;i<TITLE_FRAMES;i++) {
        pad.buttons=(uint16_t)(0x1000+(i&1));pw_wine_set_pad(&pad);
        assert(pw_wine_pad(&seen) && seen.buttons==pad.buttons);
        assert(!pw_wine_post_input(&e) && pw_wine_next_input(&out) && out.code==0x20);
        (void)pw_wine_rumble(&left,&right);
        assert(pw_wine_present(frame,4,3,16)==0);
        usleep(1000);
    }
    title_main_cpu_ns=thread_cpu_ns()-cpu;
    __atomic_store_n(&title_closed,1,__ATOMIC_RELEASE);
    __atomic_store_n(&title_main_done,1,__ATOMIC_RELEASE);
    return NULL;
}
typedef struct { int status; uint64_t cpu_ns; } DriverRelease;
static void *driver_release(void *arg)
{
    DriverRelease *d=arg;
    const uint64_t cpu=thread_cpu_ns();
    d->status=pw_wine_release_display();
    d->cpu_ns=thread_cpu_ns()-cpu;
    return NULL;
}

/* Vulkan takes the video output once; frames stop reaching the title. Last
 * in main: the release lasts for the process. */
static void test_display_release(void)
{
    uint32_t frame[4*3]={0};
    PwWineSinkStats s;
    int context_tag;

    pw_wine_sink_stats(&s);assert(!s.display_released);
    const int before=calls;
    assert(pw_wine_present(frame,4,3,16)==0 && calls==before+1);
    /* A refusal is returned and changes nothing; the driver may ask again. */
    pw_wine_set_display_release(release,&context_tag);
    release_status=-5;assert(pw_wine_release_display()==-5 && release_calls==1);
    assert(release_seen==&context_tag);
    pw_wine_sink_stats(&s);assert(!s.display_released);
    assert(pw_wine_present(frame,4,3,16)==0 && calls==before+2);
    /* The title's main thread uses the sink while the callback waits for
     * it, and two surfaces asking at once get one release between them. */
    pw_wine_set_display_release(title_release,NULL);
    pthread_t main_thread,drivers[2];
    DriverRelease release_of[2]={{7,0},{7,0}};
    assert(!pthread_create(&main_thread,NULL,title_main,NULL));
    for(int i=0;i<2;i++)assert(!pthread_create(&drivers[i],NULL,driver_release,&release_of[i]));
    for(int i=0;i<2;i++)assert(!pthread_join(drivers[i],NULL));
    assert(!pthread_join(main_thread,NULL));
    assert(release_of[0].status==0 && release_of[1].status==0 && title_calls==1 && title_main_done);
    /* About 200 ms passed; nobody spun through them. The callback polls
     * every millisecond like the title's, the second driver sleeps on the
     * release lock. */
    assert(title_main_cpu_ns<BUSY_LIMIT_NS);
    assert(release_of[0].cpu_ns<BUSY_LIMIT_NS && release_of[1].cpu_ns<BUSY_LIMIT_NS);
    pw_wine_sink_stats(&s);assert(s.display_released);
    /* Frames still reach the title, which shows them while no swapchain
     * presents. */
    assert(pw_wine_present(frame,4,3,16)==0 && calls==before+3+TITLE_FRAMES);
    /* Once released, the title is not asked again. */
    assert(pw_wine_release_display()==0 && title_calls==1 && release_calls==1);
}

static int audio_calls,audio_fail;
static int16_t audio_first,audio_last;
static void *audio_seen;
static int audio_sink(void *context,const int16_t *frames)
{
    audio_calls++;audio_seen=context;
    audio_first=frames[0];audio_last=frames[2*PW_WINE_AUDIO_GRAIN-1];
    return audio_fail?-3:0;
}

static void test_audio(void)
{
    int16_t grain[2*PW_WINE_AUDIO_GRAIN];
    PwWineSinkStats s;
    int tag;
    for(int i=0;i<2*PW_WINE_AUDIO_GRAIN;i++)grain[i]=(int16_t)(i-100);

    /* No sink: nothing is available and a grain is dropped. */
    assert(!pw_wine_audio_available());
    assert(pw_wine_audio_output(grain)==-1 && audio_calls==0);
    pw_wine_set_audio_sink(audio_sink,&tag);
    assert(pw_wine_audio_available());
    assert(pw_wine_audio_output(grain)==0 && audio_calls==1);
    assert(audio_seen==&tag && audio_first==-100 && audio_last==2*PW_WINE_AUDIO_GRAIN-101);
    assert(pw_wine_audio_output(NULL)==-1 && audio_calls==1);
    /* The sink's failure is returned and counted. */
    audio_fail=1;assert(pw_wine_audio_output(grain)==-3 && audio_calls==2);audio_fail=0;
    pw_wine_sink_stats(&s);
    assert(s.grains==1 && s.grains_dropped==3);
    /* Audio is counted apart from frames. */
    assert(s.frames==1 && s.frames_dropped==5);
    pw_wine_set_audio_sink(NULL,NULL);
    assert(!pw_wine_audio_available() && pw_wine_audio_output(grain)==-1 && audio_calls==2);
    pw_wine_sink_stats(NULL);
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
    test_audio();

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
    test_display_release();
    return 0;
}
