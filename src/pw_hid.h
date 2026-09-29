/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_HID_H
#define PW_HID_H
/*
 * A USB keyboard and mouse as Wine input: the keyboard's report (USB HID
 * usages of the keyboard page and its modifier bits) becomes Windows virtual
 * key presses and releases by difference with the previous report, and the
 * mouse's buttons become button presses and releases. The console part that
 * reads the devices is native/pw_hid_ps5.c.
 */
#include <stddef.h>
#include <stdint.h>

enum {
    PW_HID_KEYS = 16,         /* keys the PS5's keyboard state carries */
    PW_HID_MOUSE_LEFT = 1u, PW_HID_MOUSE_RIGHT = 2u, PW_HID_MOUSE_MIDDLE = 4u,
};

/* A keyboard report: modifier bits (bit 0 left Ctrl, 1 left Shift, 2 left
 * Alt, 3 left GUI, 4..7 the right ones) and up to PW_HID_KEYS pressed usages. */
typedef struct PwHidKeyboard {
    uint8_t modifiers;
    uint8_t keys[PW_HID_KEYS];
} PwHidKeyboard;

/* One Wine key or button change. */
typedef struct PwHidEvent {
    uint8_t kind;             /* 1 key, 2 mouse button */
    uint8_t down;
    uint16_t code;            /* key: Windows virtual key; button: 0 left, 1 right, 2 middle */
} PwHidEvent;

enum { PW_HID_EVENT_KEY = 1, PW_HID_EVENT_BUTTON = 2 };

/* The Windows virtual key of a keyboard-page usage, 0 when it has none. */
uint16_t pw_hid_usage_vk(uint8_t usage);

/* The changes from previous to current, releases first: at most
 * 2 * (PW_HID_KEYS + 8) events; returns how many were written. */
size_t pw_hid_keyboard_events(const PwHidKeyboard *previous, const PwHidKeyboard *current,
                              PwHidEvent *out, size_t capacity);

/* The mouse button changes between two button masks (PW_HID_MOUSE_*). */
size_t pw_hid_mouse_button_events(uint32_t previous, uint32_t current, PwHidEvent *out,
                                  size_t capacity);
#endif
