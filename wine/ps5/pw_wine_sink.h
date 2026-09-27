/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_WINE_SINK_H
#define PW_WINE_SINK_H
#include <stdint.h>

/* The meeting point between the title and Wine's PS5 user driver. ntdll
 * links it: the title reaches the set/post calls through ntdll.prx's
 * descriptor before __wine_main, and win32u's driver reaches the present
 * and next calls with dlsym. Frames go out through one sink, the title's
 * pw_present_frame path; input comes in through a bounded queue the title
 * fills from the DualSense and the driver drains in ProcessEvents. A
 * game's XInput controller is one more slot: the title keeps the newest
 * gamepad state in it, Wine's xinput reads it (its Unix library, patch
 * 0440, finds these calls with dlsym) and leaves the rumble it asks for.
 * Sound goes out like frames: Wine's audio driver (wine/wineps5) hands its
 * mix to the title's one audio port, one grain at a time. */

/* BGRA, top-down; stride in bytes. Returns the sink's result. */
typedef int (*PwWinePresentSink)(void *context,const void *bgra,uint32_t width,uint32_t height,
                                 uint32_t stride);

enum { PW_WINE_INPUT_KEY=1,PW_WINE_INPUT_MOUSE_MOVE,PW_WINE_INPUT_MOUSE_BUTTON,
       PW_WINE_INPUT_QUEUE=256 };
typedef struct PwWineInput {
    uint32_t type;
    uint32_t code;      /* KEY: Windows virtual key; MOUSE_BUTTON: 0 left, 1 right, 2 middle */
    int32_t x,y;        /* MOUSE_MOVE: absolute desktop position */
    uint32_t down;      /* KEY, MOUSE_BUTTON: 1 pressed, 0 released */
} PwWineInput;

/* The console's output format: interleaved left,right 16-bit frames at
 * 48 kHz, played PW_WINE_AUDIO_GRAIN frames at a time. */
enum { PW_WINE_AUDIO_RATE=48000,PW_WINE_AUDIO_GRAIN=256 };
/* Plays one grain and returns once the console has taken it, so the
 * console's output clocks the caller. Returns the sink's result. */
typedef int (*PwWineAudioSink)(void *context,const int16_t *frames);

typedef struct PwWineSinkStats {
    uint64_t frames,frames_dropped;       /* dropped: no sink set, or it failed */
    uint64_t inputs_posted,inputs_dropped,inputs_delivered;
    uint64_t grains,grains_dropped;       /* audio; dropped as for frames */
} PwWineSinkStats;

/* An XInput gamepad (XINPUT_GAMEPAD with the connection and a packet
 * number in front), laid out as xinput's Unix library reads it. */
typedef struct PwWinePad {
    uint32_t connected;     /* 0: no controller */
    uint32_t packet;        /* the sink's: changes whenever the state does */
    uint16_t buttons;       /* XINPUT_GAMEPAD_* */
    uint8_t left_trigger,right_trigger;
    int16_t thumb_lx,thumb_ly,thumb_rx,thumb_ry;
} PwWinePad;

/* Title side. */
void pw_wine_set_present_sink(PwWinePresentSink sink,void *context);
void pw_wine_set_audio_sink(PwWineAudioSink sink,void *context);
/* 0, or -1 when the queue is full or the event is invalid. */
int pw_wine_post_input(const PwWineInput *event);
void pw_wine_sink_stats(PwWineSinkStats *stats);
/* The gamepad's newest state, or NULL when it is disconnected. The sink
 * numbers the packets; the pad's own packet field is ignored. */
void pw_wine_set_pad(const PwWinePad *pad);
/* 1 and the newest motor speeds (0..65535) when they changed since the
 * last call, else 0. */
int pw_wine_rumble(uint32_t *left,uint32_t *right);

/* Driver side. -1 when no sink is set or the arguments are invalid. */
int pw_wine_present(const void *bgra,uint32_t width,uint32_t height,uint32_t stride);
/* 1 and the oldest event, or 0 when the queue is empty. */
int pw_wine_next_input(PwWineInput *event);
/* A non-blocking pipe that becomes readable when input is posted, so the
 * driver can have the server wake a thread idle in GetMessage. Readers
 * drain it before emptying the queue, which loses no wakeup. -1 if no
 * pipe could be created. */
int pw_wine_input_fd(void);
/* 1 and the gamepad's state while one is connected, else 0. */
int pw_wine_pad(PwWinePad *pad);
/* The rumble a game asked for (XInputSetState); speeds are clamped. */
void pw_wine_set_rumble(uint32_t left,uint32_t right);
/* 1 when the title set an audio sink: the audio driver offers its device
 * only then. */
int pw_wine_audio_available(void);
/* Blocks while the sink plays one grain. -1 when no sink is set or frames
 * is NULL. */
int pw_wine_audio_output(const int16_t *frames);

#endif
