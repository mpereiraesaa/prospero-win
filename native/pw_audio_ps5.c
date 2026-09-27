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
 * call fails, so the first result is kept and returned to later callers. */
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

int pw_audio_ps5_open_port(const PwAudioPs5Ops *ops,int *handle)
{
    int32_t volumes[8];
    if(!ops || !handle || !ops->init || !ops->open || !ops->volume || !ops->close)
        return PW_ERR_PRECONDITION;
    if(ops->init()<0)return PW_ERR_STATE;
    int port=ops->open(PW_AUDIO_PS5_USER_SYSTEM,PW_AUDIO_PS5_PORT_MAIN,
        PW_AUDIO_PS5_PORT_INDEX,PW_AUDIO_PS5_GRAIN,PW_AUDIO_PS5_RATE,
        PW_AUDIO_PS5_FORMAT_S16_STEREO);
    if(port<0)return PW_ERR_STATE;
    for(unsigned i=0;i<8;i++)volumes[i]=PW_AUDIO_PS5_VOLUME_0DB;
    if(ops->volume(port,PW_AUDIO_PS5_VOLUME_FLAGS,volumes)<0) {
        (void)ops->close(port);return PW_ERR_STATE;
    }
    *handle=port;
    return PW_OK;
}
