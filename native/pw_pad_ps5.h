/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_PAD_PS5_H
#define PW_PAD_PS5_H
#include "../src/pw_pad.h"
#include <stdint.h>

enum { PW_PAD_PS5_BATCH=64,PW_PAD_PS5_USERS=4 };
/* The touchpad's click, and the x that splits it in halves: touches report
 * x from 0 to 1919 across the pad. */
enum { PW_PAD_PS5_TOUCHPAD=0x100000,PW_PAD_PS5_TOUCH_MIDDLE=960 };
enum { PW_PAD_PS5_TOUCH_FROM_NONE,PW_PAD_PS5_TOUCH_FROM_CLICK,PW_PAD_PS5_TOUCH_FROM_EARLIER };
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
    /* Optional: sceUserServiceGetLoginUserIdList, the signed-in users' ids
     * (-1 for an empty slot), NULL if none. */
    int (*login_users)(int32_t ids[PW_PAD_PS5_USERS]);
    /* Optional: scePadSetVibrationMode(handle, mode). A DualSense opens in
     * advanced haptics mode, where scePadSetVibration's motor levels move
     * nothing; called with PW_PAD_PS5_VIBRATION_COMPATIBLE right after open
     * so the classic dual-motor path actually drives the actuators. NULL
     * if unavailable; its result is not fatal to opening the pad. */
    int (*pad_set_vibration_mode)(int32_t,int32_t);
} PwPadPs5Ops;

enum { PW_PAD_PS5_VIBRATION_COMPATIBLE=2 };

typedef struct PwPadPs5 {
    PwPad core;PwPadPs5Ops ops;int32_t user_id,pad_handle;
    int user_initialize_rc,pad_init_rc,close_rc,terminate_rc,last_read_rc;
    int vibration_rc;
    uint64_t polls,empty_reads,read_errors,connected_samples,disconnected_samples,
             intercepted_samples,generation_changes;
    uint8_t last_generation;
    unsigned owns_user_service,opened,generation_valid;
    /* 1 while the newest sample is intercepted: the system has the pad
     * (the PS button's menu, Create's capture menu). The controller is
     * still there; core.connected is 0 and its buttons are released. */
    unsigned intercepted;
    /* The newest connected sample's sticks (0..255, 0x80 centred) and
     * triggers; centred and released while disconnected or intercepted. */
    PwPadPs5Stick left_stick,right_stick;uint8_t l2,r2;
    /* Where the held touchpad click began: touch_left when its finger was
     * on the left half, touch_x and touch_y that finger's position, kept
     * until the click is released, so sliding the finger does not change
     * it. A click's own sample can come without a touch (touch_fingers 0):
     * the finger's last position before it, while it stayed down, stands
     * in; with neither, the middle, the right half. touch_from says which:
     * PW_PAD_PS5_TOUCH_FROM_*. */
    unsigned touch_held,touch_left,touch_from,touch_fingers;uint16_t touch_x,touch_y;
    /* the finger's latest position while one is on the touchpad */
    unsigned finger_down;uint16_t finger_x,finger_y;
} PwPadPs5;

int pw_pad_ps5_platform_ops(PwPadPs5Ops *);
int pw_pad_ps5_open(PwPadPs5 *,const PwPadPs5Ops *,const PwPadKeyMap *,size_t);
/* The pad of a signed-in user other than first's, for a second local
 * player: the first such user after `after` in the sign-in list, going
 * round (after<0: from the start), so a caller whose pick has no controller
 * can try the next user. first must be open; the user service stays
 * first's. PW_OK, PW_ERR_UNSUPPORTED without a login_users op,
 * PW_ERR_NOT_FOUND when no other user is signed in, PW_ERR_STATE when that
 * user's pad does not open. */
int pw_pad_ps5_open_other(PwPadPs5 *,const PwPadPs5 *first,int32_t after,
                          const PwPadKeyMap *,size_t);
/* Held buttons, edges, sticks and triggers from the pending samples. */
int pw_pad_ps5_read(PwPadPs5 *);
int pw_pad_ps5_close(PwPadPs5 *);
/* Run the motors: large is the low-frequency one (XInput's left), small
 * the high-frequency one, 0 stops. PW_OK, PW_ERR_UNSUPPORTED with no
 * vibration op, PW_ERR_PRECONDITION when the pad is not open, PW_ERR_STATE
 * when the call fails (its result is kept in vibration_rc). */
int pw_pad_ps5_vibrate(PwPadPs5 *,uint8_t large,uint8_t small);

#endif
