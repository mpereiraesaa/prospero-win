/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_audio_mix.h"
#include <string.h>

#define ONE ((uint64_t)1 << 32)
#define SIDE 0.70710678f    /* -3 dB: a speaker folded into both sides, or a surround one into its side */

/* WAVEFORMATEXTENSIBLE speaker positions. */
enum {
    FRONT_LEFT=0x1,FRONT_RIGHT=0x2,FRONT_CENTER=0x4,LOW_FREQUENCY=0x8,
    BACK_LEFT=0x10,BACK_RIGHT=0x20,FRONT_LEFT_OF_CENTER=0x40,FRONT_RIGHT_OF_CENTER=0x80,
    BACK_CENTER=0x100,SIDE_LEFT=0x200,SIDE_RIGHT=0x400,TOP_CENTER=0x800,
    TOP_FRONT_LEFT=0x1000,TOP_FRONT_CENTER=0x2000,TOP_FRONT_RIGHT=0x4000,
    TOP_BACK_LEFT=0x8000,TOP_BACK_CENTER=0x10000,TOP_BACK_RIGHT=0x20000,
};
#define LEFT_SIDE (BACK_LEFT|FRONT_LEFT_OF_CENTER|SIDE_LEFT|TOP_FRONT_LEFT|TOP_BACK_LEFT)
#define RIGHT_SIDE (BACK_RIGHT|FRONT_RIGHT_OF_CENTER|SIDE_RIGHT|TOP_FRONT_RIGHT|TOP_BACK_RIGHT)
#define CENTER (FRONT_CENTER|BACK_CENTER|TOP_CENTER|TOP_FRONT_CENTER|TOP_BACK_CENTER)

/* The layout a format without a usable mask has (Wine's OSS driver, get_channel_mask). */
static const uint32_t usual_mask[PW_AUDIO_MIX_MAX_CHANNELS+1]={
    0,FRONT_CENTER,FRONT_LEFT|FRONT_RIGHT,FRONT_LEFT|FRONT_RIGHT|LOW_FREQUENCY,
    FRONT_LEFT|FRONT_RIGHT|BACK_LEFT|BACK_RIGHT,
    FRONT_LEFT|FRONT_RIGHT|BACK_LEFT|BACK_RIGHT|LOW_FREQUENCY,
    FRONT_LEFT|FRONT_RIGHT|FRONT_CENTER|LOW_FREQUENCY|BACK_LEFT|BACK_RIGHT,
    FRONT_LEFT|FRONT_RIGHT|FRONT_CENTER|LOW_FREQUENCY|BACK_LEFT|BACK_RIGHT|BACK_CENTER,
    FRONT_LEFT|FRONT_RIGHT|FRONT_CENTER|LOW_FREQUENCY|BACK_LEFT|BACK_RIGHT|SIDE_LEFT|SIDE_RIGHT,
};

static unsigned bit_count(uint32_t mask)
{
    unsigned count=0;
    for(;mask;mask&=mask-1)count++;
    return count;
}

int pw_audio_mix_format_check(const PwAudioMixFormat *format)
{
    if(!format)return PW_ERR_PRECONDITION;
    if(format->channels<1 || format->channels>PW_AUDIO_MIX_MAX_CHANNELS ||
       format->rate<PW_AUDIO_MIX_MIN_RATE || format->rate>PW_AUDIO_MIX_MAX_RATE)
        return PW_ERR_UNSUPPORTED;
    if(format->encoding==PW_AUDIO_MIX_PCM) {
        if(format->bits!=8 && format->bits!=16 && format->bits!=24 && format->bits!=32)
            return PW_ERR_UNSUPPORTED;
    } else if(format->encoding!=PW_AUDIO_MIX_FLOAT || format->bits!=32)
        return PW_ERR_UNSUPPORTED;
    return format->block_align==format->channels*(format->bits/8u)?PW_OK:PW_ERR_UNSUPPORTED;
}

/* The weights folding each channel into the left and right outputs, times its gain. */
static void fold_weights(PwAudioMixStream *stream)
{
    const PwAudioMixFormat *format=&stream->format;
    uint32_t mask=format->channel_mask;
    if(bit_count(mask)<format->channels)mask=usual_mask[format->channels];
    for(unsigned channel=0;channel<format->channels;channel++) {
        uint32_t speaker=mask&-mask;
        float left=0,right=0;
        mask&=mask-1;
        if(format->channels==1)left=right=1;
        else if(speaker==FRONT_LEFT)left=1;
        else if(speaker==FRONT_RIGHT)right=1;
        else if(speaker&LEFT_SIDE)left=SIDE;
        else if(speaker&RIGHT_SIDE)right=SIDE;
        else if(speaker&CENTER)left=right=SIDE;
        /* the low-frequency channel is not folded in: speakers reproduce
         * what they can of the others' bass */
        stream->left[channel]=left*stream->gain[channel];
        stream->right[channel]=right*stream->gain[channel];
    }
}

int pw_audio_mix_stream_init(PwAudioMixStream *stream,const PwAudioMixFormat *format,
                             const uint8_t *frames,uint32_t capacity)
{
    if(!stream || !frames || !capacity)return PW_ERR_PRECONDITION;
    int status=pw_audio_mix_format_check(format);
    if(status!=PW_OK)return status;
    memset(stream,0,sizeof(*stream));
    stream->format=*format;stream->frames=frames;stream->capacity=capacity;
    for(unsigned channel=0;channel<PW_AUDIO_MIX_MAX_CHANNELS;channel++)stream->gain[channel]=1;
    fold_weights(stream);
    pw_audio_mix_stream_reset(stream);
    return pw_audio_mix_set_rate(stream,format->rate);
}

void pw_audio_mix_stream_reset(PwAudioMixStream *stream)
{
    if(!stream)return;
    stream->offset=stream->held=0;stream->consumed=0;
    stream->phase=ONE;      /* the first output frame reads one */
    stream->previous[0]=stream->previous[1]=stream->current[0]=stream->current[1]=0;
}

int pw_audio_mix_set_rate(PwAudioMixStream *stream,uint32_t rate)
{
    if(!stream)return PW_ERR_PRECONDITION;
    if(rate<PW_AUDIO_MIX_MIN_RATE || rate>PW_AUDIO_MIX_MAX_RATE)return PW_ERR_UNSUPPORTED;
    stream->format.rate=rate;
    stream->step=((uint64_t)rate<<32)/PW_AUDIO_MIX_RATE;
    return PW_OK;
}

int pw_audio_mix_set_gain(PwAudioMixStream *stream,const float *gain,uint32_t count)
{
    if(!stream || !gain || count!=stream->format.channels)return PW_ERR_PRECONDITION;
    for(uint32_t channel=0;channel<count;channel++) {
        float value=gain[channel];
        stream->gain[channel]=value>1?1:value>0?value:0;
    }
    fold_weights(stream);
    return PW_OK;
}

static float sample(const uint8_t *at,const PwAudioMixFormat *format)
{
    int32_t value;
    float result;
    switch(format->bits) {
    case 8:return ((int)at[0]-128)*(1.0f/128);
    case 16:return (int16_t)(uint16_t)(at[0]|at[1]<<8)*(1.0f/32768);
    case 24:
        value=(int32_t)((uint32_t)at[0]<<8|(uint32_t)at[1]<<16|(uint32_t)at[2]<<24);
        return (float)(value>>8)*(1.0f/8388608);
    default:
        if(format->encoding==PW_AUDIO_MIX_FLOAT) {
            memcpy(&result,at,sizeof(result));
            /* full scale is +-1; a NaN or infinity would poison the mix */
            return result>1?1:result<-1?-1:result==result?result:0;
        }
        value=(int32_t)((uint32_t)at[0]|(uint32_t)at[1]<<8|(uint32_t)at[2]<<16|(uint32_t)at[3]<<24);
        return (float)value*(1.0f/2147483648.0f);
    }
}

/* Take the oldest held frame, folded to stereo, as the resampler's current one. */
static void take_frame(PwAudioMixStream *stream)
{
    const PwAudioMixFormat *format=&stream->format;
    const uint8_t *at=stream->frames+(size_t)stream->offset*format->block_align;
    float left=0,right=0;
    for(unsigned channel=0;channel<format->channels;channel++) {
        float value=sample(at+channel*(format->bits/8u),format);
        left+=value*stream->left[channel];right+=value*stream->right[channel];
    }
    stream->previous[0]=stream->current[0];stream->previous[1]=stream->current[1];
    stream->current[0]=left;stream->current[1]=right;
    if(++stream->offset==stream->capacity)stream->offset=0;
    stream->held--;stream->consumed++;
}

uint32_t pw_audio_mix_add(PwAudioMixStream *stream,float *accumulator,uint32_t frames)
{
    uint32_t done=0;
    if(!stream || !accumulator)return 0;
    for(;done<frames;done++) {
        while(stream->phase>=ONE) {
            if(!stream->held)return done;
            take_frame(stream);
            stream->phase-=ONE;
        }
        float t=(float)(uint32_t)stream->phase*(1.0f/4294967296.0f);
        accumulator[2*done]+=stream->previous[0]+(stream->current[0]-stream->previous[0])*t;
        accumulator[2*done+1]+=stream->previous[1]+(stream->current[1]-stream->previous[1])*t;
        stream->phase+=stream->step;
    }
    return done;
}

void pw_audio_mix_store(const float *accumulator,int16_t *output,uint32_t frames)
{
    if(!accumulator || !output)return;
    for(uint32_t i=0;i<2*frames;i++) {
        float value=accumulator[i];
        value=value>1?1:value<-1?-1:value;
        value*=32767;
        output[i]=(int16_t)(value>=0?value+0.5f:value-0.5f);
    }
}
