/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_AUDIO_PS5_H
#define PW_AUDIO_PS5_H
/*
 * The console's main audio port, for a caller that mixes and clocks
 * itself: Wine's audio driver (wine/wineps5) hands the title one grain of
 * 16-bit stereo at a time, and each output call returns when the port has
 * taken it.
 */
#include "../include/prospero_win.h"
#include <stdint.h>

enum { PW_AUDIO_PS5_RATE=48000, PW_AUDIO_PS5_GRAIN=256, PW_AUDIO_PS5_CHANNELS=2 };

typedef struct PwAudioPs5Ops {
    int (*init)(void);
    int (*open)(int user_id,int type,int index,uint32_t grain,
                uint32_t rate,uint32_t format);
    int (*volume)(int handle,int flags,const int32_t *volumes);
    int (*output)(int handle,const void *samples);
    int (*close)(int handle);
} PwAudioPs5Ops;

int pw_audio_ps5_platform_ops(PwAudioPs5Ops *);
/* The main port for a caller that mixes and clocks itself (Wine's audio
 * sink): the library initialised, PW_AUDIO_PS5_GRAIN frames of 16-bit
 * stereo at PW_AUDIO_PS5_RATE per output call, unity volume. The caller
 * then calls ops->output(*handle, grain) and ops->close(*handle).
 * PW_ERR_STATE when the library refuses. */
int pw_audio_ps5_open_port(const PwAudioPs5Ops *ops,int *handle);

#endif
