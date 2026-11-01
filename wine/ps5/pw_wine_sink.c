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
static pthread_mutex_t release_lock=PTHREAD_MUTEX_INITIALIZER;
static PwWineInput queue[PW_WINE_INPUT_QUEUE];
static uint32_t head,count;
static PwWineSinkStats stats;
static pthread_once_t wake_once=PTHREAD_ONCE_INIT;
static int wake[2]={-1,-1};
static PwWinePad pad_state[PW_WINE_PADS];
static PwWinePadCaps pad_caps[PW_WINE_PADS];
static uint32_t pad_changes;
static uint32_t rumble_left[PW_WINE_PADS],rumble_right[PW_WINE_PADS],rumble_changed[PW_WINE_PADS];
static uint32_t pads_wanted;
static struct pw_title_menu menu_state={.version=PW_TITLE_MENU_VERSION};
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
/* A full pipe is already readable: nothing is lost. */
static void wake_driver(void)
{
    char byte=1;
    pthread_once(&wake_once,create_wake_pipe);
    if(wake[1]!=-1 && write(wake[1],&byte,1)<0){}
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
    PwWineDisplayRelease release;
    void *context;
    int status=0,released;
    /* The title's callback waits for its main thread to close the video
     * output, and that thread posts input and the pad's state here while it
     * runs (the pad every frame): the callback must not hold the lock they
     * take, or the main thread blocks until the title gives up. Releases
     * are serialised by a lock of their own, so a second caller waits for
     * the first one's answer. */
    pthread_mutex_lock(&release_lock);
    pthread_mutex_lock(&lock);
    released=stats.display_released;release=display_release;context=display_context;
    pthread_mutex_unlock(&lock);
    if(!released && release)status=release(context);
    if(!status) {
        pthread_mutex_lock(&lock);
        stats.display_released=1;
        pthread_mutex_unlock(&lock);
    }
    pthread_mutex_unlock(&release_lock);
    return status;
}
int pw_wine_present(const void *bgra,uint32_t width,uint32_t height,uint32_t stride)
{
    int status=-1;
    /* The lock also serialises frames: the sink sees one at a time. */
    pthread_mutex_lock(&lock);
    if(sink && bgra && width && height && stride>=width*4u)
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
    case PW_WINE_INPUT_MOUSE_WHEEL:return e->x!=0 && e->code==0 && e->y==0 && e->down==0;
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
    if(!status)wake_driver();
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
static int same_caps(const PwWinePadCaps *a,const PwWinePadCaps *b)
{
    return a->subtype==b->subtype && a->vendor_id==b->vendor_id &&
           a->product_id==b->product_id && a->revision==b->revision && a->flags==b->flags;
}
int pw_wine_set_pad_device_at(uint32_t index,const PwWinePad *pad,const PwWinePadCaps *caps)
{
    static const PwWinePadCaps legacy={1,0x045e,0x028e,0x0114,PW_WINE_PAD_FFB};
    int changed,identity;
    if(index>=PW_WINE_PADS)return -1;
    if(!caps)caps=&legacy;
    PwWinePad *state=&pad_state[index];
    pthread_mutex_lock(&lock);
    identity=pad && state->connected && !same_caps(caps,&pad_caps[index]);
    changed=((pad!=NULL)!=(state->connected!=0)) || identity;
    if(changed)__atomic_add_fetch(&pad_changes,1,__ATOMIC_RELAXED);
    if(!pad) {
        state->connected=0;
        memset(&pad_caps[index],0,sizeof(pad_caps[index]));
        if(rumble_left[index] || rumble_right[index])rumble_changed[index]=1;
        rumble_left[index]=rumble_right[index]=0;
    }
    else if(!state->connected || identity || !same_pad(pad,state)) {
        uint32_t packet=state->packet+1;
        *state=*pad;state->connected=1;
        state->packet=packet?packet:1;   /* 0 is never a packet */
        pad_caps[index]=*caps;
    }
    pthread_mutex_unlock(&lock);
    if(changed)wake_driver();   /* the driver tells the game's windows */
    return 0;
}
void pw_wine_set_pad_at(uint32_t index,const PwWinePad *pad)
{
    (void)pw_wine_set_pad_device_at(index,pad,NULL);
}
int pw_wine_pad_caps_at(uint32_t index,PwWinePadCaps *caps)
{
    int connected;
    if(!caps)return 0;
    memset(caps,0,sizeof(*caps));
    if(index>=PW_WINE_PADS)return 0;
    pthread_mutex_lock(&lock);
    connected=pad_state[index].connected!=0;
    if(connected)*caps=pad_caps[index];
    pthread_mutex_unlock(&lock);
    return connected;
}
/* No lock: the driver asks at every ProcessEvents. */
uint32_t pw_wine_pad_changes(void)
{
    return __atomic_load_n(&pad_changes,__ATOMIC_RELAXED);
}
void pw_wine_set_pad(const PwWinePad *pad){pw_wine_set_pad_at(0,pad);}
int pw_wine_pad_at(uint32_t index,PwWinePad *pad)
{
    int connected;
    if(!pad)return 0;
    if(index>=PW_WINE_PADS){memset(pad,0,sizeof(*pad));return 0;}
    pthread_mutex_lock(&lock);
    pads_wanted|=1u<<index;
    connected=pad_state[index].connected!=0;
    if(connected)*pad=pad_state[index];
    else memset(pad,0,sizeof(*pad));
    pthread_mutex_unlock(&lock);
    return connected;
}
int pw_wine_pad(PwWinePad *pad){return pw_wine_pad_at(0,pad);}
void pw_wine_set_rumble_at(uint32_t index,uint32_t left,uint32_t right)
{
    if(index>=PW_WINE_PADS)return;
    if(left>0xffffu)left=0xffffu;
    if(right>0xffffu)right=0xffffu;
    pthread_mutex_lock(&lock);
    if(left!=rumble_left[index] || right!=rumble_right[index]) {
        rumble_left[index]=left;rumble_right[index]=right;rumble_changed[index]=1;
    }
    pthread_mutex_unlock(&lock);
}
void pw_wine_set_rumble(uint32_t left,uint32_t right){pw_wine_set_rumble_at(0,left,right);}
int pw_wine_rumble_at(uint32_t index,uint32_t *left,uint32_t *right)
{
    int changed;
    if(index>=PW_WINE_PADS)return 0;
    pthread_mutex_lock(&lock);
    changed=(int)rumble_changed[index];rumble_changed[index]=0;
    if(left)*left=rumble_left[index];
    if(right)*right=rumble_right[index];
    pthread_mutex_unlock(&lock);
    return changed;
}
int pw_wine_rumble(uint32_t *left,uint32_t *right){return pw_wine_rumble_at(0,left,right);}
uint32_t pw_wine_pads_wanted(void)
{
    uint32_t wanted;
    pthread_mutex_lock(&lock);
    wanted=pads_wanted;
    pthread_mutex_unlock(&lock);
    return wanted;
}

void pw_wine_set_menu(const struct pw_title_menu *menu)
{
    struct pw_title_menu next={.version=PW_TITLE_MENU_VERSION};
    if(menu && menu->open) {
        next.open=1;
        next.lines=menu->lines<PW_TITLE_MENU_LINES?menu->lines:PW_TITLE_MENU_LINES;
        next.selected=menu->selected>=0 && (uint32_t)menu->selected<next.lines?menu->selected:-1;
        memcpy(next.title,menu->title,sizeof(next.title));
        next.title[sizeof(next.title)-1]=0;
        for(uint32_t i=0;i<next.lines;i++) {
            memcpy(next.line[i],menu->line[i],sizeof(next.line[i]));
            next.line[i][sizeof(next.line[i])-1]=0;
        }
    }
    pthread_mutex_lock(&lock);
    next.sequence=menu_state.sequence;
    if(memcmp(&next,&menu_state,sizeof(next))) {
        next.sequence=menu_state.sequence+1;
        menu_state=next;
    }
    pthread_mutex_unlock(&lock);
}
void pw_wine_menu(struct pw_title_menu *menu)
{
    if(!menu)return;
    pthread_mutex_lock(&lock);
    *menu=menu_state;
    pthread_mutex_unlock(&lock);
}
