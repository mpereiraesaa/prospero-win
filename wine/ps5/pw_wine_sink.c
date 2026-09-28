/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_wine_sink.h"
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static PwWinePresentSink sink;
static void *sink_context;
static PwWineDisplayRelease display_release;
static void *display_context;
static PwWineInput queue[PW_WINE_INPUT_QUEUE];
static uint32_t head,count;
static PwWineSinkStats stats;
static pthread_once_t wake_once=PTHREAD_ONCE_INIT;
static int wake[2]={-1,-1};
static PwWinePad pad_state;
static uint32_t rumble_left,rumble_right,rumble_changed;
/* Audio has its own lock: its sink blocks for a grain, which must not hold
 * up frames or input. */
static pthread_mutex_t audio_lock=PTHREAD_MUTEX_INITIALIZER;
static PwWineAudioSink audio_sink;
static void *audio_context;
static uint64_t grains,grains_dropped;

static void create_wake_pipe(void)
{
    int pair[2];
    if(pipe(pair))return;
    for(int i=0;i<2;i++) {
        fcntl(pair[i],F_SETFL,fcntl(pair[i],F_GETFL)|O_NONBLOCK);
        fcntl(pair[i],F_SETFD,FD_CLOEXEC);
    }
    wake[0]=pair[0];wake[1]=pair[1];
}
int pw_wine_input_fd(void)
{
    pthread_once(&wake_once,create_wake_pipe);
    return wake[0];
}

void pw_wine_set_present_sink(PwWinePresentSink new_sink,void *context)
{
    pthread_mutex_lock(&lock);
    sink=new_sink;sink_context=context;
    pthread_mutex_unlock(&lock);
}
void pw_wine_set_display_release(PwWineDisplayRelease release,void *context)
{
    pthread_mutex_lock(&lock);
    display_release=release;display_context=context;
    pthread_mutex_unlock(&lock);
}
int pw_wine_release_display(void)
{
    int status=0;
    /* Under the frame lock: no frame is being shown while the title lets go. */
    pthread_mutex_lock(&lock);
    if(!stats.display_released && display_release)status=display_release(display_context);
    if(!status)stats.display_released=1;
    pthread_mutex_unlock(&lock);
    return status;
}
int pw_wine_present(const void *bgra,uint32_t width,uint32_t height,uint32_t stride)
{
    int status=-1;
    /* The lock also serialises frames: the sink sees one at a time. */
    pthread_mutex_lock(&lock);
    if(sink && !stats.display_released && bgra && width && height && stride>=width*4u)
        status=sink(sink_context,bgra,width,height,stride);
    if(status)stats.frames_dropped++;
    else stats.frames++;
    pthread_mutex_unlock(&lock);
    return status;
}

static int valid(const PwWineInput *e)
{
    switch(e->type) {
    case PW_WINE_INPUT_KEY:return e->code>0 && e->code<256 && e->down<=1;
    case PW_WINE_INPUT_MOUSE_MOVE:return 1;
    case PW_WINE_INPUT_MOUSE_BUTTON:return e->code<=2 && e->down<=1;
    default:return 0;
    }
}
int pw_wine_post_input(const PwWineInput *event)
{
    int status=-1;
    pthread_mutex_lock(&lock);
    if(event && valid(event) && count<PW_WINE_INPUT_QUEUE) {
        queue[(head+count)%PW_WINE_INPUT_QUEUE]=*event;count++;
        stats.inputs_posted++;status=0;
    }
    else stats.inputs_dropped++;
    pthread_mutex_unlock(&lock);
    if(!status) {
        char byte=1;
        pthread_once(&wake_once,create_wake_pipe);
        /* a full pipe is already readable: nothing is lost */
        if(wake[1]!=-1 && write(wake[1],&byte,1)<0){}
    }
    return status;
}
int pw_wine_next_input(PwWineInput *event)
{
    int found=0;
    if(!event)return 0;
    pthread_mutex_lock(&lock);
    if(count) {
        *event=queue[head];head=(head+1)%PW_WINE_INPUT_QUEUE;count--;
        stats.inputs_delivered++;found=1;
    }
    pthread_mutex_unlock(&lock);
    return found;
}
void pw_wine_set_audio_sink(PwWineAudioSink new_sink,void *context)
{
    pthread_mutex_lock(&audio_lock);
    audio_sink=new_sink;audio_context=context;
    pthread_mutex_unlock(&audio_lock);
}
int pw_wine_audio_available(void)
{
    pthread_mutex_lock(&audio_lock);
    int available=audio_sink!=NULL;
    pthread_mutex_unlock(&audio_lock);
    return available;
}
int pw_wine_audio_output(const int16_t *frames)
{
    int status=-1;
    /* The lock also serialises grains: the sink sees one at a time. */
    pthread_mutex_lock(&audio_lock);
    if(audio_sink && frames)status=audio_sink(audio_context,frames);
    if(status)grains_dropped++;
    else grains++;
    pthread_mutex_unlock(&audio_lock);
    return status;
}

void pw_wine_sink_stats(PwWineSinkStats *out)
{
    if(!out)return;
    pthread_mutex_lock(&lock);
    *out=stats;
    pthread_mutex_unlock(&lock);
    pthread_mutex_lock(&audio_lock);
    out->grains=grains;out->grains_dropped=grains_dropped;
    pthread_mutex_unlock(&audio_lock);
}

/* The pad has its own fields compared, not memcmp'd: padding stays out. */
static int same_pad(const PwWinePad *a,const PwWinePad *b)
{
    return a->buttons==b->buttons && a->left_trigger==b->left_trigger &&
           a->right_trigger==b->right_trigger && a->thumb_lx==b->thumb_lx &&
           a->thumb_ly==b->thumb_ly && a->thumb_rx==b->thumb_rx && a->thumb_ry==b->thumb_ry;
}
void pw_wine_set_pad(const PwWinePad *pad)
{
    pthread_mutex_lock(&lock);
    if(!pad)pad_state.connected=0;
    else if(!pad_state.connected || !same_pad(pad,&pad_state)) {
        uint32_t packet=pad_state.packet+1;
        pad_state=*pad;pad_state.connected=1;
        pad_state.packet=packet?packet:1;   /* 0 is never a packet */
    }
    pthread_mutex_unlock(&lock);
}
int pw_wine_pad(PwWinePad *pad)
{
    int connected;
    if(!pad)return 0;
    pthread_mutex_lock(&lock);
    connected=pad_state.connected!=0;
    if(connected)*pad=pad_state;
    else memset(pad,0,sizeof(*pad));
    pthread_mutex_unlock(&lock);
    return connected;
}
void pw_wine_set_rumble(uint32_t left,uint32_t right)
{
    if(left>0xffffu)left=0xffffu;
    if(right>0xffffu)right=0xffffu;
    pthread_mutex_lock(&lock);
    if(left!=rumble_left || right!=rumble_right) {
        rumble_left=left;rumble_right=right;rumble_changed=1;
    }
    pthread_mutex_unlock(&lock);
}
int pw_wine_rumble(uint32_t *left,uint32_t *right)
{
    int changed;
    pthread_mutex_lock(&lock);
    changed=(int)rumble_changed;rumble_changed=0;
    if(left)*left=rumble_left;
    if(right)*right=rumble_right;
    pthread_mutex_unlock(&lock);
    return changed;
}
