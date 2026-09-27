/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_AUDIO_PS5_H
#define PW_AUDIO_PS5_H
#include "../src/pw_win32.h"
#include <pthread.h>
#include <stdint.h>

enum { PW_AUDIO_PS5_RATE=48000, PW_AUDIO_PS5_GRAIN=256,
       PW_AUDIO_PS5_CHANNELS=2, PW_AUDIO_PS5_QUEUE_BLOCKS=1024,
       PW_AUDIO_PS5_COMPLETIONS=64 };

typedef struct PwAudioPs5Ops {
    int (*init)(void);
    int (*open)(int user_id,int type,int index,uint32_t grain,
                uint32_t rate,uint32_t format);
    int (*volume)(int handle,int flags,const int32_t *volumes);
    int (*output)(int handle,const void *samples);
    int (*close)(int handle);
} PwAudioPs5Ops;

typedef struct PwAudioPs5Block {
    int16_t samples[PW_AUDIO_PS5_GRAIN*PW_AUDIO_PS5_CHANNELS];
    uint32_t completion_token,completion_bytes,generation;
} PwAudioPs5Block;

typedef struct PwAudioPs5Completion { uint32_t token,bytes; } PwAudioPs5Completion;

typedef struct PwAudioPs5Stats {
    uint64_t input_bytes,output_frames,blocks,enqueues,completions;
    uint64_t queue_full,output_errors;
    uint32_t input_hash,queue_depth,queue_high_water;
    unsigned opened,paused,worker_running;
} PwAudioPs5Stats;

typedef struct PwAudioPs5 {
    PwAudioPs5Ops ops;
    PwAudioPs5Block *queue;
    uint32_t queue_capacity,queue_head,queue_count,queue_high_water;
    PwAudioPs5Completion completed[PW_AUDIO_PS5_COMPLETIONS];
    uint32_t completed_head,completed_count;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t worker;
    int handle,worker_error,init_rc,open_rc;
    uint32_t input_rate,phase,generation;
    uint16_t input_channels,input_bits;
    uint64_t input_bytes,output_frames,blocks,enqueues,completions;
    uint64_t queue_full,output_errors;
    uint32_t input_hash;
    unsigned opened,paused,active,stopping,worker_running,initialized;
} PwAudioPs5;

/* queue_storage remains caller-owned until close. Submit copies and converts
 * guest PCM into it; only the worker is allowed to call SceAudioOut. */
int pw_audio_ps5_init(PwAudioPs5 *,const PwAudioPs5Ops *,
                      PwAudioPs5Block *queue_storage,uint32_t queue_capacity);
int pw_audio_ps5_open(void *,uint32_t,uint16_t,uint16_t);
int pw_audio_ps5_submit(void *,const void *,uint32_t,uint32_t completion_token);
int pw_audio_ps5_poll(void *,uint32_t *completion_token,uint32_t *bytes);
int pw_audio_ps5_control(void *,PwAudioControl);
int pw_audio_ps5_stats(PwAudioPs5 *,PwAudioPs5Stats *);
int pw_audio_ps5_close(PwAudioPs5 *);
int pw_audio_ps5_platform_ops(PwAudioPs5Ops *);
/* The main port for a caller that mixes and clocks itself (Wine's audio
 * sink): the library initialised, PW_AUDIO_PS5_GRAIN frames of 16-bit
 * stereo at PW_AUDIO_PS5_RATE per output call, unity volume. The caller
 * then calls ops->output(*handle, grain) and ops->close(*handle).
 * PW_ERR_STATE when the library refuses. */
int pw_audio_ps5_open_port(const PwAudioPs5Ops *ops,int *handle);

#endif
