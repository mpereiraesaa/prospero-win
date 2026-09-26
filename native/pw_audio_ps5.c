/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_audio_ps5.h"
#include <string.h>

enum { PW_AUDIO_PS5_USER_SYSTEM=0xff, PW_AUDIO_PS5_PORT_MAIN=0,
       PW_AUDIO_PS5_PORT_INDEX=0, PW_AUDIO_PS5_FORMAT_S16_STEREO=1,
       PW_AUDIO_PS5_VOLUME_FLAGS=3, PW_AUDIO_PS5_VOLUME_0DB=0x8000 };

#ifndef PW_AUDIO_PS5_HOST_TEST
extern int sceAudioOutInit(void);
extern int sceAudioOutOpen(int,int,int,uint32_t,uint32_t,uint32_t);
extern int sceAudioOutSetVolume(int,int,const int32_t *);
extern int sceAudioOutOutput(int,const void *);
extern int sceAudioOutClose(int);
/* sceAudioOutInit initialises the library for the whole process; a second
 * call fails. A launcher opens audio once per session, so the first result
 * is kept and returned to every later session. */
static int platform_init(void)
{
    static int initialized,result;
    if(!initialized){result=sceAudioOutInit();initialized=result>=0;}
    return result;
}
static int platform_open(int u,int t,int i,uint32_t g,uint32_t r,uint32_t f)
{return sceAudioOutOpen(u,t,i,g,r,f);}
static int platform_volume(int h,int f,const int32_t *v)
{return sceAudioOutSetVolume(h,f,v);}
static int platform_output(int h,const void *p){return sceAudioOutOutput(h,p);}
static int platform_close(int h){return sceAudioOutClose(h);}
#endif

int pw_audio_ps5_platform_ops(PwAudioPs5Ops *ops)
{
    if(!ops)return PW_ERR_PRECONDITION;
#ifdef PW_AUDIO_PS5_HOST_TEST
    memset(ops,0,sizeof(*ops));return PW_ERR_UNSUPPORTED;
#else
    *ops=(PwAudioPs5Ops){platform_init,platform_open,platform_volume,
                         platform_output,platform_close};
    return PW_OK;
#endif
}

static void completed_push(PwAudioPs5 *audio,uint32_t token,uint32_t bytes)
{
    if(!token)return;
    if(audio->completed_count==PW_AUDIO_PS5_COMPLETIONS) {
        audio->worker_error=PW_ERR_LIMIT;return;
    }
    uint32_t tail=(audio->completed_head+audio->completed_count)%PW_AUDIO_PS5_COMPLETIONS;
    audio->completed[tail]=(PwAudioPs5Completion){token,bytes};
    audio->completed_count++;audio->completions++;
}

static void *audio_worker(void *opaque)
{
    PwAudioPs5 *audio=opaque;
    pthread_mutex_lock(&audio->mutex);
    audio->worker_running=1;pthread_cond_broadcast(&audio->condition);
    while(!audio->stopping) {
        while(!audio->stopping && (!audio->opened || audio->paused || !audio->queue_count))
            pthread_cond_wait(&audio->condition,&audio->mutex);
        if(audio->stopping)break;
        PwAudioPs5Block block=audio->queue[audio->queue_head];
        audio->queue_head=(audio->queue_head+1)%audio->queue_capacity;
        audio->queue_count--;audio->active=1;
        int handle=audio->handle;
        pthread_cond_broadcast(&audio->condition);
        pthread_mutex_unlock(&audio->mutex);
        int result=audio->ops.output(handle,block.samples);
        pthread_mutex_lock(&audio->mutex);
        audio->active=0;
        if(result<0) {
            audio->output_errors++;audio->worker_error=PW_ERR_STATE;
        } else {
            audio->blocks++;audio->output_frames+=PW_AUDIO_PS5_GRAIN;
            if(block.generation==audio->generation)
                completed_push(audio,block.completion_token,block.completion_bytes);
        }
        pthread_cond_broadcast(&audio->condition);
    }
    audio->worker_running=0;pthread_cond_broadcast(&audio->condition);
    pthread_mutex_unlock(&audio->mutex);return NULL;
}

int pw_audio_ps5_init(PwAudioPs5 *audio,const PwAudioPs5Ops *ops,
                      PwAudioPs5Block *queue_storage,uint32_t queue_capacity)
{
    if(!audio || !ops || !queue_storage || !queue_capacity || !ops->init ||
       !ops->open || !ops->volume || !ops->output || !ops->close)
        return PW_ERR_PRECONDITION;
    memset(audio,0,sizeof(*audio));audio->ops=*ops;audio->handle=-1;
    audio->queue=queue_storage;audio->queue_capacity=queue_capacity;audio->generation=1;
    if(pthread_mutex_init(&audio->mutex,NULL))return PW_ERR_STATE;
    if(pthread_cond_init(&audio->condition,NULL)) {
        pthread_mutex_destroy(&audio->mutex);return PW_ERR_STATE;
    }
    audio->initialized=1;
    if(pthread_create(&audio->worker,NULL,audio_worker,audio)) {
        audio->initialized=0;pthread_cond_destroy(&audio->condition);
        pthread_mutex_destroy(&audio->mutex);return PW_ERR_STATE;
    }
    pthread_mutex_lock(&audio->mutex);
    while(!audio->worker_running)pthread_cond_wait(&audio->condition,&audio->mutex);
    pthread_mutex_unlock(&audio->mutex);return PW_OK;
}

int pw_audio_ps5_open(void *opaque,uint32_t rate,uint16_t channels,uint16_t bits)
{
    PwAudioPs5 *audio=opaque;int32_t volumes[8];
    if(!audio || !audio->initialized || !rate || (channels!=1 && channels!=2) ||
       (bits!=8 && bits!=16))return PW_ERR_PRECONDITION;
    pthread_mutex_lock(&audio->mutex);unsigned open=audio->opened;
    pthread_mutex_unlock(&audio->mutex);if(open)return PW_ERR_STATE;
    int result=audio->ops.init();audio->init_rc=result;if(result<0)return PW_ERR_STATE;
    result=audio->ops.open(PW_AUDIO_PS5_USER_SYSTEM,PW_AUDIO_PS5_PORT_MAIN,
        PW_AUDIO_PS5_PORT_INDEX,PW_AUDIO_PS5_GRAIN,PW_AUDIO_PS5_RATE,
        PW_AUDIO_PS5_FORMAT_S16_STEREO);
    audio->open_rc=result;if(result<0)return PW_ERR_STATE;
    for(unsigned i=0;i<8;i++)volumes[i]=PW_AUDIO_PS5_VOLUME_0DB;
    if(audio->ops.volume(result,PW_AUDIO_PS5_VOLUME_FLAGS,volumes)<0) {
        (void)audio->ops.close(result);return PW_ERR_STATE;
    }
    pthread_mutex_lock(&audio->mutex);
    audio->handle=result;audio->input_rate=rate;audio->input_channels=channels;
    audio->input_bits=bits;audio->phase=0;audio->paused=0;audio->opened=1;
    audio->worker_error=PW_OK;pthread_cond_broadcast(&audio->condition);
    pthread_mutex_unlock(&audio->mutex);return PW_OK;
}

static int16_t sample(const uint8_t *input,uint16_t bits)
{
    if(bits==8)return (int16_t)(((int)input[0]-128)<<8);
    return (int16_t)((uint16_t)input[0]|(uint16_t)input[1]<<8);
}

static uint32_t output_frames(uint32_t phase,uint32_t input_frames,uint32_t rate)
{
    return (uint32_t)(((uint64_t)phase+(uint64_t)input_frames*PW_AUDIO_PS5_RATE)/rate);
}

int pw_audio_ps5_submit(void *opaque,const void *pcm,uint32_t bytes,uint32_t token)
{
    PwAudioPs5 *audio=opaque;const uint8_t *input=pcm;
    if(!audio || !audio->initialized || !input || !bytes || !token)
        return PW_ERR_PRECONDITION;
    pthread_mutex_lock(&audio->mutex);
    if(!audio->opened || audio->worker_error!=PW_OK) {
        pthread_mutex_unlock(&audio->mutex);return PW_ERR_STATE;
    }
    uint32_t frame_bytes=(uint32_t)audio->input_channels*(audio->input_bits/8u);
    if(bytes%frame_bytes) {
        pthread_mutex_unlock(&audio->mutex);return PW_ERR_PRECONDITION;
    }
    uint32_t frames=output_frames(audio->phase,bytes/frame_bytes,audio->input_rate);
    uint32_t required=(frames+PW_AUDIO_PS5_GRAIN-1)/PW_AUDIO_PS5_GRAIN;
    if(!required || required>audio->queue_capacity-audio->queue_count) {
        audio->queue_full++;pthread_mutex_unlock(&audio->mutex);return PW_ERR_LIMIT;
    }
    uint32_t hash=audio->input_hash?audio->input_hash:2166136261u;
    for(uint32_t i=0;i<bytes;i++){hash^=input[i];hash*=16777619u;}
    uint32_t tail=(audio->queue_head+audio->queue_count)%audio->queue_capacity;
    uint32_t block_frame=0,created=0;
    memset(&audio->queue[tail],0,sizeof(audio->queue[tail]));
    for(uint32_t offset=0;offset<bytes;offset+=frame_bytes) {
        int16_t left=sample(input+offset,audio->input_bits);
        int16_t right=audio->input_channels==2?
            sample(input+offset+audio->input_bits/8u,audio->input_bits):left;
        audio->phase+=PW_AUDIO_PS5_RATE;
        while(audio->phase>=audio->input_rate) {
            audio->phase-=audio->input_rate;
            audio->queue[tail].samples[block_frame*2]=left;
            audio->queue[tail].samples[block_frame*2+1]=right;
            if(++block_frame==PW_AUDIO_PS5_GRAIN) {
                created++;audio->queue_count++;block_frame=0;
                if(created<required) {
                    tail=(tail+1)%audio->queue_capacity;
                    memset(&audio->queue[tail],0,sizeof(audio->queue[tail]));
                }
            }
        }
    }
    if(block_frame){created++;audio->queue_count++;}
    if(created!=required) {
        audio->worker_error=PW_ERR_STATE;pthread_mutex_unlock(&audio->mutex);
        return PW_ERR_STATE;
    }
    audio->queue[tail].completion_token=token;
    audio->queue[tail].completion_bytes=bytes;
    uint32_t first=(tail+audio->queue_capacity+1-required)%audio->queue_capacity;
    for(uint32_t i=0;i<required;i++)
        audio->queue[(first+i)%audio->queue_capacity].generation=audio->generation;
    audio->input_hash=hash;audio->input_bytes+=bytes;audio->enqueues++;
    if(audio->queue_count>audio->queue_high_water)audio->queue_high_water=audio->queue_count;
    pthread_cond_broadcast(&audio->condition);
    pthread_mutex_unlock(&audio->mutex);return PW_OK;
}

int pw_audio_ps5_poll(void *opaque,uint32_t *token,uint32_t *bytes)
{
    PwAudioPs5 *audio=opaque;
    if(!audio || !audio->initialized || !token || !bytes)return PW_ERR_PRECONDITION;
    pthread_mutex_lock(&audio->mutex);
    if(audio->worker_error!=PW_OK) {
        int status=audio->worker_error;pthread_mutex_unlock(&audio->mutex);return status;
    }
    if(!audio->completed_count) {
        pthread_mutex_unlock(&audio->mutex);return PW_ERR_NOT_FOUND;
    }
    *token=audio->completed[audio->completed_head].token;
    *bytes=audio->completed[audio->completed_head].bytes;
    audio->completed_head=(audio->completed_head+1)%PW_AUDIO_PS5_COMPLETIONS;
    audio->completed_count--;
    pthread_mutex_unlock(&audio->mutex);return PW_OK;
}

int pw_audio_ps5_control(void *opaque,PwAudioControl control)
{
    PwAudioPs5 *audio=opaque;
    if(!audio || !audio->initialized)return PW_ERR_PRECONDITION;
    pthread_mutex_lock(&audio->mutex);
    if(!audio->opened){pthread_mutex_unlock(&audio->mutex);return PW_ERR_PRECONDITION;}
    if(control==PW_AUDIO_PAUSE)audio->paused=1;
    else if(control==PW_AUDIO_RESTART)audio->paused=0;
    else if(control==PW_AUDIO_RESET) {
        audio->generation++;if(!audio->generation)audio->generation=1;
        audio->queue_head=audio->queue_count=0;audio->completed_head=audio->completed_count=0;
        audio->phase=0;audio->paused=0;
    } else if(control==PW_AUDIO_CLOSE) {
        if(audio->active || audio->queue_count) {
            pthread_mutex_unlock(&audio->mutex);return PW_ERR_STATE;
        }
        int handle=audio->handle;audio->opened=0;audio->handle=-1;
        pthread_mutex_unlock(&audio->mutex);
        return audio->ops.close(handle)<0?PW_ERR_STATE:PW_OK;
    } else {pthread_mutex_unlock(&audio->mutex);return PW_ERR_UNSUPPORTED;}
    pthread_cond_broadcast(&audio->condition);
    pthread_mutex_unlock(&audio->mutex);return PW_OK;
}

int pw_audio_ps5_stats(PwAudioPs5 *audio,PwAudioPs5Stats *stats)
{
    if(!audio || !audio->initialized || !stats)return PW_ERR_PRECONDITION;
    pthread_mutex_lock(&audio->mutex);
    *stats=(PwAudioPs5Stats){
        .input_bytes=audio->input_bytes,.output_frames=audio->output_frames,
        .blocks=audio->blocks,.enqueues=audio->enqueues,.completions=audio->completions,
        .queue_full=audio->queue_full,.output_errors=audio->output_errors,
        .input_hash=audio->input_hash,
        .queue_depth=audio->queue_count+(audio->active?1u:0u),
        .queue_high_water=audio->queue_high_water,.opened=audio->opened,
        .paused=audio->paused,.worker_running=audio->worker_running};
    pthread_mutex_unlock(&audio->mutex);return PW_OK;
}

int pw_audio_ps5_close(PwAudioPs5 *audio)
{
    if(!audio || !audio->initialized)return PW_ERR_PRECONDITION;
    pthread_mutex_lock(&audio->mutex);
    audio->stopping=1;audio->queue_count=0;audio->completed_count=0;
    pthread_cond_broadcast(&audio->condition);
    pthread_mutex_unlock(&audio->mutex);
    int status=pthread_join(audio->worker,NULL)?PW_ERR_STATE:PW_OK;
    if(audio->opened && audio->ops.close(audio->handle)<0)status=PW_ERR_STATE;
    pthread_cond_destroy(&audio->condition);pthread_mutex_destroy(&audio->mutex);
    audio->opened=0;audio->initialized=0;audio->handle=-1;return status;
}
