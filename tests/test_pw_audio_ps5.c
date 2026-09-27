/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../native/pw_audio_ps5.h"
#include <assert.h>
#include <sched.h>
#include <string.h>

static unsigned inits,opens,volumes,outputs,closes;
static int16_t last[PW_AUDIO_PS5_GRAIN*2];
static pthread_mutex_t output_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t output_condition=PTHREAD_COND_INITIALIZER;
static unsigned hold_output;

static int op_init(void){inits++;return 0;}
static int op_open(int u,int t,int i,uint32_t g,uint32_t r,uint32_t f)
{assert(u==0xff&&t==0&&i==0&&g==256&&r==48000&&f==1);opens++;return 7;}
static int op_volume(int h,int flags,const int32_t *v)
{assert(h==7&&flags==3&&v[0]==0x8000&&v[7]==0x8000);volumes++;return 0;}
static int op_output(int h,const void *p)
{
    assert(h==7);memcpy(last,p,sizeof(last));
    pthread_mutex_lock(&output_mutex);outputs++;
    pthread_cond_broadcast(&output_condition);
    while(hold_output)pthread_cond_wait(&output_condition,&output_mutex);
    pthread_mutex_unlock(&output_mutex);return 0;
}
static int op_close(int h){assert(h==7);closes++;return 0;}

static void wait_for_outputs(unsigned wanted)
{
    pthread_mutex_lock(&output_mutex);
    while(outputs<wanted)pthread_cond_wait(&output_condition,&output_mutex);
    pthread_mutex_unlock(&output_mutex);
}

static int fail_init(void){inits++;return -1;}
static int fail_open(int u,int t,int i,uint32_t g,uint32_t r,uint32_t f)
{(void)u;(void)t;(void)i;(void)g;(void)r;(void)f;opens++;return -2;}
static int fail_volume(int h,int flags,const int32_t *v)
{(void)flags;(void)v;assert(h==7);volumes++;return -3;}

/* The bare port Wine's audio sink plays grains on. */
static void test_open_port(void)
{
    PwAudioPs5Ops ops={op_init,op_open,op_volume,op_output,op_close};
    int handle=-1;
    inits=opens=volumes=outputs=closes=0;
    assert(pw_audio_ps5_open_port(NULL,&handle)==PW_ERR_PRECONDITION);
    assert(pw_audio_ps5_open_port(&ops,NULL)==PW_ERR_PRECONDITION);
    PwAudioPs5Ops partial=ops;partial.volume=NULL;
    assert(pw_audio_ps5_open_port(&partial,&handle)==PW_ERR_PRECONDITION);
    assert(inits==0 && handle==-1);
    assert(pw_audio_ps5_open_port(&ops,&handle)==PW_OK);
    assert(handle==7 && inits==1 && opens==1 && volumes==1 && closes==0);
    int16_t grain[PW_AUDIO_PS5_GRAIN*2];
    for(int i=0;i<PW_AUDIO_PS5_GRAIN*2;i++)grain[i]=(int16_t)i;
    assert(ops.output(handle,grain)==0 && outputs==1 && last[511]==511);

    handle=-1;
    PwAudioPs5Ops broken=ops;broken.init=fail_init;
    assert(pw_audio_ps5_open_port(&broken,&handle)==PW_ERR_STATE && opens==1 && handle==-1);
    broken=ops;broken.open=fail_open;
    assert(pw_audio_ps5_open_port(&broken,&handle)==PW_ERR_STATE && volumes==1 && handle==-1);
    /* a port whose volume cannot be set is closed again */
    broken=ops;broken.volume=fail_volume;
    assert(pw_audio_ps5_open_port(&broken,&handle)==PW_ERR_STATE && closes==1 && handle==-1);
    inits=opens=volumes=outputs=closes=0;
}

int main(void)
{
    test_open_port();
    static PwAudioPs5Block queue[PW_AUDIO_PS5_QUEUE_BLOCKS];
    PwAudioPs5 audio;PwAudioPs5Ops ops={op_init,op_open,op_volume,op_output,op_close};
    assert(pw_audio_ps5_init(&audio,&ops,queue,PW_AUDIO_PS5_QUEUE_BLOCKS)==PW_OK);
    assert(pw_audio_ps5_open(&audio,11025,1,8)==PW_OK);
    assert(audio.init_rc==0 && audio.open_rc==7);

    uint8_t pcm[256];memset(pcm,255,sizeof(pcm));
    pthread_mutex_lock(&output_mutex);hold_output=1;pthread_mutex_unlock(&output_mutex);
    assert(pw_audio_ps5_submit(&audio,pcm,sizeof(pcm),0x1234)==PW_OK);
    /* Submission is a copy/enqueue operation: it does not wait for output. */
    wait_for_outputs(1);
    uint32_t token=0,bytes=0;
    assert(pw_audio_ps5_poll(&audio,&token,&bytes)==PW_ERR_NOT_FOUND);
    pthread_mutex_lock(&output_mutex);hold_output=0;
    pthread_cond_broadcast(&output_condition);pthread_mutex_unlock(&output_mutex);
    for(unsigned attempt=0;attempt<1000;attempt++) {
        if(pw_audio_ps5_poll(&audio,&token,&bytes)==PW_OK)break;
        sched_yield();
    }
    assert(token==0x1234 && bytes==sizeof(pcm));
    assert(last[0]==32512 && last[1]==32512);

    PwAudioPs5Stats stats;
    assert(pw_audio_ps5_stats(&audio,&stats)==PW_OK);
    assert(stats.blocks>=4 && stats.output_frames==stats.blocks*256u);
    assert(stats.input_bytes==256 && stats.input_hash==0x06a34ac5u);
    assert(stats.enqueues==1 && stats.completions==1 && stats.queue_high_water>=4);

    assert(pw_audio_ps5_control(&audio,PW_AUDIO_PAUSE)==PW_OK);
    assert(pw_audio_ps5_control(&audio,PW_AUDIO_RESTART)==PW_OK);
    assert(pw_audio_ps5_control(&audio,PW_AUDIO_CLOSE)==PW_OK);
    assert(pw_audio_ps5_close(&audio)==PW_OK);
    assert(inits==1&&opens==1&&volumes==1&&closes==1&&!audio.opened);
    pthread_cond_destroy(&output_condition);pthread_mutex_destroy(&output_mutex);
    return 0;
}
