/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_PAD_PS5_H
#define PW_PAD_PS5_H
#include "../src/pw_pad.h"
#include <stdint.h>

enum { PW_PAD_PS5_BATCH=64 };
typedef struct PwPadPs5Stick { uint8_t x,y; } PwPadPs5Stick;
typedef struct PwPadPs5Touch { uint16_t x,y;uint8_t finger,reserved[3]; } PwPadPs5Touch;
typedef struct PwPadPs5TouchData {
    uint8_t fingers,reserved0[3];uint32_t reserved1;PwPadPs5Touch touch[2];
} PwPadPs5TouchData;
typedef struct PwPadPs5Data {
    uint32_t buttons;PwPadPs5Stick left_stick,right_stick;uint8_t l2,r2;uint16_t reserved0;
    float quaternion[4],acceleration[3],angular_velocity[3];PwPadPs5TouchData touch_data;
    int32_t connected;uint64_t timestamp;uint8_t extension[16],connected_count,reserved1[2];
    uint8_t device_unique_data_length,device_unique_data[12];
} PwPadPs5Data;

typedef struct PwPadPs5Ops {
    int (*user_initialize)(const void *);
    int (*foreground_user)(int32_t *);
    int (*user_terminate)(void);
    int (*pad_init)(void);
    int (*pad_open)(int32_t,int32_t,int32_t,const void *);
    int (*pad_read)(int32_t,PwPadPs5Data *,int32_t);
    int (*pad_close)(int32_t);
    /* Optional: scePadSetVibration(handle, {large, small}), NULL if none. */
    int (*pad_set_vibration)(int32_t,const uint8_t motors[2]);
} PwPadPs5Ops;

typedef struct PwPadPs5 {
    PwPad core;PwPadPs5Ops ops;int32_t user_id,pad_handle;
    int user_initialize_rc,pad_init_rc,close_rc,terminate_rc,last_read_rc;
    int vibration_rc;
    uint64_t polls,empty_reads,read_errors,connected_samples,disconnected_samples,
             intercepted_samples,generation_changes;
    uint8_t last_generation;
    unsigned owns_user_service,opened,generation_valid;
    /* The newest connected sample's sticks (0..255, 0x80 centred) and
     * triggers; centred and released while disconnected or intercepted. */
    PwPadPs5Stick left_stick,right_stick;uint8_t l2,r2;
} PwPadPs5;

int pw_pad_ps5_platform_ops(PwPadPs5Ops *);
int pw_pad_ps5_open(PwPadPs5 *,const PwPadPs5Ops *,const PwPadKeyMap *,size_t);
int pw_pad_ps5_poll(PwPadPs5 *,PwUser32 *,uint32_t window);
/* The same read for a native UI: held buttons and edges, no Win32 keys. */
int pw_pad_ps5_read(PwPadPs5 *);
int pw_pad_ps5_close(PwPadPs5 *,PwUser32 *,uint32_t window);
/* Run the motors: large is the low-frequency one (XInput's left), small
 * the high-frequency one, 0 stops. PW_OK, PW_ERR_UNSUPPORTED with no
 * vibration op, PW_ERR_PRECONDITION when the pad is not open, PW_ERR_STATE
 * when the call fails (its result is kept in vibration_rc). */
int pw_pad_ps5_vibrate(PwPadPs5 *,uint8_t large,uint8_t small);

#endif
