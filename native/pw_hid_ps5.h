/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_HID_PS5_H
#define PW_HID_PS5_H
/*
 * A USB keyboard and mouse on the PS5, read through the system's
 * libSceKeyboard and libSceMouse, which the system loads as its modules
 * (sceSysmoduleLoadModule: a title may not load them by path, 0x80020063,
 * measured) and whose functions are then looked up (the payload SDK has no
 * libSceMouse stub). The layouts are those public PS4/PS5
 * reimplementations agree on (shadPS4, GPCS4, fpPS4, OpenOrbis, and the PS5
 * ports that ran on hardware: ps5-payload-dev's SDL, PS5SX2):
 *
 *   mouse record, 40 bytes: u64 timestamp; u8 connected (+3); u32 buttons;
 *     s32 x, y (motion since the last record), wheel, tilt; u8 reserve[8]
 *   keyboard state, 96 bytes: u64 timestamp; 8 bytes; u8 connected (+3);
 *     s32 key count; u32 locks; u32 modifiers (the USB boot report's bits);
 *     u16 USB HID usages[16]; 32 reserved bytes
 *
 * The first states and records that carry data are logged in hex, so a
 * layout the firmware does not share shows at once.
 */
#include "../src/pw_hid.h"
#include <stddef.h>
#include <stdint.h>

enum {
    PW_HID_PS5_MOUSE_RECORD = 40, PW_HID_PS5_MOUSE_RECORDS = 64,
    PW_HID_PS5_KEYBOARD_STATE = 96, PW_HID_PS5_DUMPS = 3,
    PW_HID_PS5_EVENTS = 2 * (PW_HID_KEYS + 8) + 3,
};

typedef struct PwHidPs5Ops {
    int32_t (*load_module)(const char *name);                 /* "libSceMouse": handle, or < 0 */
    int (*resolve)(int32_t module, const char *name, void **address);
    void (*log)(const char *line);
} PwHidPs5Ops;

typedef struct PwHidPs5 {
    PwHidPs5Ops ops;
    int32_t user_id;
    int32_t keyboard, mouse;                                  /* handles; -1 when absent */
    int keyboard_failed, mouse_failed;                        /* the last open failed (logged) */
    int (*keyboard_read_state)(int32_t handle, void *state);
    int (*mouse_read)(int32_t handle, void *records, int32_t count);
    PwHidKeyboard keys;                                       /* the last keyboard state */
    uint32_t buttons;                                         /* the last mouse buttons */
    uint64_t mouse_time;                                      /* newest record seen */
    unsigned keyboard_dumps, mouse_dumps;
} PwHidPs5;

/* One poll: key and button changes, and the mouse's motion. */
typedef struct PwHidPs5Poll {
    int32_t dx, dy, wheel;
    size_t count;
    PwHidEvent events[PW_HID_PS5_EVENTS];
} PwHidPs5Poll;

/* Opens whichever of the keyboard and mouse the system has for user_id;
 * logs each step. PW_OK when either opened. */
int pw_hid_ps5_open(PwHidPs5 *hid, const PwHidPs5Ops *ops, int32_t user_id);
/* Opens again whichever of the two is not open, as when it was plugged in
 * after the start; a repeated failure is not logged again. PW_OK when
 * either is open. */
int pw_hid_ps5_retry(PwHidPs5 *hid);
/* Reads both devices once. */
void pw_hid_ps5_poll(PwHidPs5 *hid, PwHidPs5Poll *poll);

/* The layouts, for tests: a keyboard state (0 when disconnected), and the
 * motion and newest buttons of count mouse records. */
int pw_hid_ps5_keyboard_state(const uint8_t *state, PwHidKeyboard *out);
int pw_hid_ps5_mouse_records(const uint8_t *records, int count, int32_t *dx, int32_t *dy,
                             int32_t *wheel, uint32_t *buttons, uint64_t *newest);

/* The system's functions, on the console. */
const PwHidPs5Ops *pw_hid_ps5_system_ops(void (*log)(const char *line));
/* Loads both modules early: before the title's /data grant, since a title
 * whose credentials changed is reported to fail to load system modules. */
void pw_hid_ps5_preload(void (*log)(const char *line));
#endif
