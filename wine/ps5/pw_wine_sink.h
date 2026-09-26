/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_WINE_SINK_H
#define PW_WINE_SINK_H
#include <stdint.h>

/* The meeting point between the title and Wine's PS5 user driver. ntdll
 * links it: the title reaches the set/post calls through ntdll.prx's
 * descriptor before __wine_main, and win32u's driver reaches the present
 * and next calls with dlsym. Frames go out through one sink, the title's
 * pw_present_frame path; input comes in through a bounded queue the title
 * fills from the DualSense and the driver drains in ProcessEvents. */

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

typedef struct PwWineSinkStats {
    uint64_t frames,frames_dropped;       /* dropped: no sink set, or it failed */
    uint64_t inputs_posted,inputs_dropped,inputs_delivered;
} PwWineSinkStats;

/* Title side. */
void pw_wine_set_present_sink(PwWinePresentSink sink,void *context);
/* 0, or -1 when the queue is full or the event is invalid. */
int pw_wine_post_input(const PwWineInput *event);
void pw_wine_sink_stats(PwWineSinkStats *stats);

/* Driver side. -1 when no sink is set or the arguments are invalid. */
int pw_wine_present(const void *bgra,uint32_t width,uint32_t height,uint32_t stride);
/* 1 and the oldest event, or 0 when the queue is empty. */
int pw_wine_next_input(PwWineInput *event);

#endif
