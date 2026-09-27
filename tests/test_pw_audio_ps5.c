/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../native/pw_audio_ps5.h"
#include <assert.h>
#include <string.h>

static unsigned inits,opens,volumes,outputs,closes;
static int16_t last[PW_AUDIO_PS5_GRAIN*2];

static int op_init(void){inits++;return 0;}
static int op_open(int u,int t,int i,uint32_t g,uint32_t r,uint32_t f)
{assert(u==0xff&&t==0&&i==0&&g==256&&r==48000&&f==1);opens++;return 7;}
static int op_volume(int h,int flags,const int32_t *v)
{assert(h==7&&flags==3&&v[0]==0x8000&&v[7]==0x8000);volumes++;return 0;}
static int op_output(int h,const void *p)
{
    assert(h==7);memcpy(last,p,sizeof(last));outputs++;return 0;
}
static int op_close(int h){assert(h==7);closes++;return 0;}

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
    PwAudioPs5Ops ops;
    assert(pw_audio_ps5_platform_ops(NULL)==PW_ERR_PRECONDITION);
    /* The host build has no console library behind the ops. */
    assert(pw_audio_ps5_platform_ops(&ops)==PW_ERR_UNSUPPORTED && !ops.init && !ops.output);
    return 0;
}
