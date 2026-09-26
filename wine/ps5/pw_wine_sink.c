/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_wine_sink.h"
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <unistd.h>

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static PwWinePresentSink sink;
static void *sink_context;
static PwWineInput queue[PW_WINE_INPUT_QUEUE];
static uint32_t head,count;
static PwWineSinkStats stats;
static pthread_once_t wake_once=PTHREAD_ONCE_INIT;
static int wake[2]={-1,-1};

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
void pw_wine_sink_stats(PwWineSinkStats *out)
{
    if(!out)return;
    pthread_mutex_lock(&lock);
    *out=stats;
    pthread_mutex_unlock(&lock);
}
