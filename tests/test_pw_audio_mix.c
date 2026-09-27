/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_audio_mix.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int near(float a,float b){return fabsf(a-b)<1e-4f;}

static PwAudioMixFormat pcm(uint32_t rate,uint16_t channels,uint16_t bits)
{
    return (PwAudioMixFormat){rate,channels,bits,(uint16_t)(channels*bits/8u),PW_AUDIO_MIX_PCM,0};
}

/* Mix frames of one stream on its own and return the accumulator. */
static float mixed[2*64];
static uint32_t mix(PwAudioMixStream *stream,uint32_t frames)
{
    memset(mixed,0,sizeof(mixed));
    return pw_audio_mix_add(stream,mixed,frames);
}

static void test_format_check(void)
{
    PwAudioMixFormat f=pcm(48000,2,16);
    assert(pw_audio_mix_format_check(NULL)==PW_ERR_PRECONDITION);
    assert(pw_audio_mix_format_check(&f)==PW_OK);
    for(uint16_t bits=8;bits<=32;bits+=8){f=pcm(44100,1,bits);assert(pw_audio_mix_format_check(&f)==PW_OK);}
    f=pcm(48000,2,12);assert(pw_audio_mix_format_check(&f)==PW_ERR_UNSUPPORTED);
    f=pcm(48000,0,16);assert(pw_audio_mix_format_check(&f)==PW_ERR_UNSUPPORTED);
    f=pcm(48000,9,16);assert(pw_audio_mix_format_check(&f)==PW_ERR_UNSUPPORTED);
    f=pcm(999,2,16);assert(pw_audio_mix_format_check(&f)==PW_ERR_UNSUPPORTED);
    f=pcm(384001,2,16);assert(pw_audio_mix_format_check(&f)==PW_ERR_UNSUPPORTED);
    f=pcm(1000,2,16);assert(pw_audio_mix_format_check(&f)==PW_OK);
    f=pcm(384000,8,32);assert(pw_audio_mix_format_check(&f)==PW_OK);
    f=pcm(48000,2,16);f.block_align=6;assert(pw_audio_mix_format_check(&f)==PW_ERR_UNSUPPORTED);
    f=pcm(48000,2,32);f.encoding=PW_AUDIO_MIX_FLOAT;assert(pw_audio_mix_format_check(&f)==PW_OK);
    f=pcm(48000,2,16);f.encoding=PW_AUDIO_MIX_FLOAT;assert(pw_audio_mix_format_check(&f)==PW_ERR_UNSUPPORTED);
    f=pcm(48000,2,16);f.encoding=2;assert(pw_audio_mix_format_check(&f)==PW_ERR_UNSUPPORTED);
}

static void test_init_errors(void)
{
    PwAudioMixStream s;uint8_t ring[16];
    PwAudioMixFormat f=pcm(48000,2,16),bad=pcm(48000,2,12);
    assert(pw_audio_mix_stream_init(NULL,&f,ring,4)==PW_ERR_PRECONDITION);
    assert(pw_audio_mix_stream_init(&s,&f,NULL,4)==PW_ERR_PRECONDITION);
    assert(pw_audio_mix_stream_init(&s,&f,ring,0)==PW_ERR_PRECONDITION);
    assert(pw_audio_mix_stream_init(&s,NULL,ring,4)==PW_ERR_PRECONDITION);
    assert(pw_audio_mix_stream_init(&s,&bad,ring,4)==PW_ERR_UNSUPPORTED);
    assert(pw_audio_mix_stream_init(&s,&f,ring,4)==PW_OK);
    assert(s.held==0 && s.offset==0 && s.consumed==0 && s.format.rate==48000);
    assert(pw_audio_mix_add(NULL,mixed,1)==0);
    assert(pw_audio_mix_add(&s,NULL,1)==0);
    assert(mix(&s,4)==0);            /* empty: nothing to add */
    pw_audio_mix_stream_reset(NULL);
    pw_audio_mix_store(NULL,NULL,1);
    assert(pw_audio_mix_set_rate(NULL,48000)==PW_ERR_PRECONDITION);
    assert(pw_audio_mix_set_rate(&s,999)==PW_ERR_UNSUPPORTED);
    assert(pw_audio_mix_set_rate(&s,384001)==PW_ERR_UNSUPPORTED);
    assert(s.format.rate==48000);
}

/* At 48 kHz frames pass through unchanged, one frame late: the resampler
 * outputs the frame before the one it last took. */
static void test_passthrough_16(void)
{
    int16_t ring[4*2]={16384,-16384, 32767,-32768, 0,8192, -8192,0};
    PwAudioMixStream s;PwAudioMixFormat f=pcm(48000,2,16);
    assert(pw_audio_mix_stream_init(&s,&f,(const uint8_t *)ring,4)==PW_OK);
    s.held=4;
    assert(mix(&s,4)==4);
    assert(near(mixed[0],0) && near(mixed[1],0));
    assert(near(mixed[2],0.5f) && near(mixed[3],-0.5f));
    assert(near(mixed[4],32767/32768.0f) && near(mixed[5],-1));
    assert(near(mixed[6],0) && near(mixed[7],0.25f));
    assert(s.held==0 && s.consumed==4 && s.offset==0);
    /* the ring ran out: the last frame waits for more */
    assert(mix(&s,2)==0);
    assert(mixed[0]==0 && mixed[1]==0);
}

static void test_sample_formats(void)
{
    PwAudioMixStream s;PwAudioMixFormat f;
    uint8_t u8[3*2]={128,128, 255,0, 0,192};
    f=pcm(48000,2,8);
    assert(pw_audio_mix_stream_init(&s,&f,u8,3)==PW_OK);s.held=3;
    assert(mix(&s,4)==3);
    assert(near(mixed[2],0) && near(mixed[3],0));
    assert(near(mixed[4],127/128.0f) && near(mixed[5],-1));

    uint8_t s24[2*3*2]={0,0,0x40, 0,0,0xc0,  0xff,0xff,0x7f, 0x00,0x00,0x80};
    f=pcm(48000,2,24);
    assert(pw_audio_mix_stream_init(&s,&f,s24,2)==PW_OK);s.held=2;
    assert(mix(&s,3)==2);
    assert(near(mixed[2],0.5f) && near(mixed[3],-0.5f));

    int32_t s32[2*2]={0x40000000,(int32_t)0xc0000000u, 0x7fffffff,(int32_t)0x80000000u};
    f=pcm(48000,2,32);
    assert(pw_audio_mix_stream_init(&s,&f,(const uint8_t *)s32,2)==PW_OK);s.held=2;
    assert(mix(&s,3)==2);
    assert(near(mixed[2],0.5f) && near(mixed[3],-0.5f));

    float fl[3*2]={0.25f,-0.75f, NAN,INFINITY, 3.0f,-3.0f};
    f=pcm(48000,2,32);f.encoding=PW_AUDIO_MIX_FLOAT;
    assert(pw_audio_mix_stream_init(&s,&f,(const uint8_t *)fl,3)==PW_OK);s.held=3;
    assert(mix(&s,4)==3);
    assert(near(mixed[2],0.25f) && near(mixed[3],-0.75f));
    assert(mixed[4]==0 && near(mixed[5],1));      /* NaN is silent, infinity full scale */
    s.held=1;s.offset=2;
    assert(mix(&s,1)==1);                          /* the clamped frame, taken again */
    assert(near(mixed[0],1) && near(mixed[1],-1));
    assert(mix(&s,1)==0);
    s.held=0;
}

static void test_channel_folding(void)
{
    PwAudioMixStream s;PwAudioMixFormat f;
    const float side=0.70710678f;

    int16_t mono[2]={16384,0};
    f=pcm(48000,1,16);
    assert(pw_audio_mix_stream_init(&s,&f,(const uint8_t *)mono,2)==PW_OK);s.held=2;
    assert(mix(&s,2)==2);
    assert(near(mixed[2],0.5f) && near(mixed[3],0.5f));   /* mono plays on both sides at full level */

    /* 5.1: FL FR FC LFE BL BR */
    int16_t six[2*6]={16384,8192,16384,32767,8192,16384, 0,0,0,0,0,0};
    f=pcm(48000,6,16);
    assert(pw_audio_mix_stream_init(&s,&f,(const uint8_t *)six,2)==PW_OK);s.held=2;
    assert(mix(&s,2)==2);
    assert(near(mixed[2],0.5f+0.5f*side+0.25f*side));
    assert(near(mixed[3],0.25f+0.5f*side+0.5f*side));

    /* a mask naming fewer speakers than channels falls back to the usual layout */
    f.channel_mask=0x3;
    assert(pw_audio_mix_stream_init(&s,&f,(const uint8_t *)six,2)==PW_OK);s.held=2;
    assert(mix(&s,2)==2);
    assert(near(mixed[2],0.5f+0.5f*side+0.25f*side));

    /* explicit speakers: side left, side right, back centre, top front left */
    int16_t four[2*4]={16384,16384,16384,16384, 0,0,0,0};
    f=pcm(48000,4,16);f.channel_mask=0x200|0x400|0x100|0x1000;
    assert(pw_audio_mix_stream_init(&s,&f,(const uint8_t *)four,2)==PW_OK);s.held=2;
    assert(mix(&s,2)==2);
    /* bits are taken in order: BC (0x100), SL, SR, TFL */
    assert(near(mixed[2],0.5f*side*3));
    assert(near(mixed[3],0.5f*side*2));

    /* the usual 3-channel layout is FL FR LFE: the LFE is dropped */
    int16_t three[2*3]={0,0,32767, 0,0,0};
    f=pcm(48000,3,16);
    assert(pw_audio_mix_stream_init(&s,&f,(const uint8_t *)three,2)==PW_OK);s.held=2;
    assert(mix(&s,2)==2);
    assert(mixed[2]==0 && mixed[3]==0);

    /* a quad layout: FL FR BL BR */
    int16_t quad[2*4]={0,0,16384,-16384, 0,0,0,0};
    f=pcm(48000,4,16);
    assert(pw_audio_mix_stream_init(&s,&f,(const uint8_t *)quad,2)==PW_OK);s.held=2;
    assert(mix(&s,2)==2);
    assert(near(mixed[2],0.5f*side) && near(mixed[3],-0.5f*side));

    /* front left/right of centre and top back right */
    int16_t wide[2*3]={16384,16384,16384, 0,0,0};
    f=pcm(48000,3,16);f.channel_mask=0x40|0x80|0x20000;
    assert(pw_audio_mix_stream_init(&s,&f,(const uint8_t *)wide,2)==PW_OK);s.held=2;
    assert(mix(&s,2)==2);
    assert(near(mixed[2],0.5f*side) && near(mixed[3],0.5f*side*2));

    /* an unknown speaker bit plays nowhere */
    int16_t odd[2*2]={16384,16384, 0,0};
    f=pcm(48000,2,16);f.channel_mask=0x1|0x80000000u;
    assert(pw_audio_mix_stream_init(&s,&f,(const uint8_t *)odd,2)==PW_OK);s.held=2;
    assert(mix(&s,2)==2);
    assert(near(mixed[2],0.5f) && mixed[3]==0);
}

static void test_gain(void)
{
    int16_t ring[2*2]={16384,16384, 0,0};
    PwAudioMixStream s;PwAudioMixFormat f=pcm(48000,2,16);
    float half[2]={0.5f,2.0f},negative[2]={-1,0.25f};
    assert(pw_audio_mix_stream_init(&s,&f,(const uint8_t *)ring,2)==PW_OK);
    assert(pw_audio_mix_set_gain(NULL,half,2)==PW_ERR_PRECONDITION);
    assert(pw_audio_mix_set_gain(&s,NULL,2)==PW_ERR_PRECONDITION);
    assert(pw_audio_mix_set_gain(&s,half,1)==PW_ERR_PRECONDITION);
    assert(pw_audio_mix_set_gain(&s,half,2)==PW_OK);
    assert(s.gain[0]==0.5f && s.gain[1]==1);       /* clamped to unity */
    s.held=2;
    assert(mix(&s,2)==2);
    assert(near(mixed[2],0.25f) && near(mixed[3],0.5f));
    assert(pw_audio_mix_set_gain(&s,negative,2)==PW_OK);
    assert(s.gain[0]==0 && s.gain[1]==0.25f);
    s.offset=0;s.held=2;
    assert(mix(&s,2)==2);
    assert(near(mixed[2],0) && near(mixed[3],0.125f));
}

/* 24 kHz: every input frame spans two output frames, the second halfway. */
static void test_upsample(void)
{
    int16_t ring[3]={16384,-16384,0};
    PwAudioMixStream s;PwAudioMixFormat f=pcm(24000,1,16);
    assert(pw_audio_mix_stream_init(&s,&f,(const uint8_t *)ring,3)==PW_OK);
    assert(s.step==((uint64_t)1<<31));
    s.held=3;
    assert(mix(&s,6)==6);
    const float expected[6]={0,0.25f,0.5f,0,-0.5f,-0.25f};
    for(int i=0;i<6;i++)assert(near(mixed[2*i],expected[i]) && near(mixed[2*i+1],expected[i]));
    assert(s.consumed==3 && s.held==0);
    assert(mix(&s,1)==0);
}

/* 96 kHz: every other input frame is skipped over. */
static void test_downsample(void)
{
    int16_t ring[8]={0,8192,16384,8192,0,-8192,-16384,-8192};
    PwAudioMixStream s;PwAudioMixFormat f=pcm(96000,1,16);
    assert(pw_audio_mix_stream_init(&s,&f,(const uint8_t *)ring,8)==PW_OK);
    s.held=8;
    assert(mix(&s,4)==4);
    /* each output is the input one frame before its position */
    const float expected[4]={0,0.25f,0.25f,-0.25f};
    for(int i=0;i<4;i++)assert(near(mixed[2*i],expected[i]));
    assert(s.consumed==7 && s.held==1);
    assert(pw_audio_mix_set_rate(&s,48000)==PW_OK && s.step==((uint64_t)1<<32));
}

static void test_wrap_underrun_reset(void)
{
    int16_t ring[3*2]={1000,-1000, 2000,-2000, 3000,-3000};
    PwAudioMixStream s;PwAudioMixFormat f=pcm(48000,2,16);
    assert(pw_audio_mix_stream_init(&s,&f,(const uint8_t *)ring,3)==PW_OK);
    s.offset=2;s.held=3;             /* frames 2, 0, 1 */
    assert(mix(&s,2)==2);
    assert(near(mixed[2],3000/32768.0f));
    assert(s.offset==1 && s.held==1);
    assert(mix(&s,4)==1);            /* one frame left, then the ring is dry */
    assert(near(mixed[0],1000/32768.0f) && mixed[2]==0);
    assert(s.offset==2 && s.held==0);
    s.held=1;                        /* more arrives: it resumes */
    assert(mix(&s,1)==1 && near(mixed[0],2000/32768.0f));
    pw_audio_mix_stream_reset(&s);
    assert(s.offset==0 && s.held==0 && s.consumed==0);
    s.held=1;
    assert(mix(&s,1)==1 && mixed[0]==0 && mixed[1]==0);    /* the history is silence again */
}

static void test_mix_and_store(void)
{
    int16_t a[2*2]={24576,-24576, 0,0},b[2*2]={16384,-16384, 0,0};
    PwAudioMixStream sa,sb;PwAudioMixFormat f=pcm(48000,2,16);
    float sum[4]={0};
    int16_t out[4];
    assert(pw_audio_mix_stream_init(&sa,&f,(const uint8_t *)a,2)==PW_OK);sa.held=2;
    assert(pw_audio_mix_stream_init(&sb,&f,(const uint8_t *)b,2)==PW_OK);sb.held=2;
    assert(pw_audio_mix_add(&sa,sum,2)==2 && pw_audio_mix_add(&sb,sum,2)==2);
    pw_audio_mix_store(sum,out,2);
    assert(out[0]==0 && out[1]==0);             /* the history */
    assert(out[2]==32767 && out[3]==-32767);    /* 1.25 clamps */
    const float values[4]={0.5f,-0.5f,0.25f/32767,-0.75f/32767};
    pw_audio_mix_store(values,out,2);
    assert(out[0]==16384 && out[1]==-16384 && out[2]==0 && out[3]==-1);
}

int main(void)
{
    test_format_check();
    test_init_errors();
    test_passthrough_16();
    test_sample_formats();
    test_channel_folding();
    test_gain();
    test_upsample();
    test_downsample();
    test_wrap_underrun_reset();
    test_mix_and_store();
    puts("audio mix passed");
    return 0;
}
