/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_hid.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_usages(void)
{
    assert(pw_hid_usage_vk(0x04) == 'A' && pw_hid_usage_vk(0x1d) == 'Z');
    assert(pw_hid_usage_vk(0x1e) == '1' && pw_hid_usage_vk(0x27) == '0');
    assert(pw_hid_usage_vk(0x28) == 0x0d && pw_hid_usage_vk(0x29) == 0x1b && pw_hid_usage_vk(0x2c) == 0x20);
    assert(pw_hid_usage_vk(0x3a) == 0x70 && pw_hid_usage_vk(0x45) == 0x7b);          /* F1, F12 */
    assert(pw_hid_usage_vk(0x4f) == 0x27 && pw_hid_usage_vk(0x52) == 0x26);          /* Right, Up */
    assert(pw_hid_usage_vk(0x59) == 0x61 && pw_hid_usage_vk(0x62) == 0x60);          /* keypad 1, 0 */
    assert(!pw_hid_usage_vk(0) && !pw_hid_usage_vk(1) && !pw_hid_usage_vk(0x66) && !pw_hid_usage_vk(0xe0));
}

static void test_keyboard(void)
{
    PwHidKeyboard a = { 0 }, b = { 0 };
    PwHidEvent e[32];
    size_t n;

    /* Press A, then A and S, then only S: one press each, one release. */
    b.keys[0] = 0x04;
    n = pw_hid_keyboard_events(&a, &b, e, 32);
    assert(n == 1 && e[0].kind == PW_HID_EVENT_KEY && e[0].down && e[0].code == 'A');
    a = b; b.keys[1] = 0x16;
    n = pw_hid_keyboard_events(&a, &b, e, 32);
    assert(n == 1 && e[0].down && e[0].code == 'S');
    a = b; memset(b.keys, 0, sizeof(b.keys)); b.keys[0] = 0x16;   /* S moved to slot 0 */
    n = pw_hid_keyboard_events(&a, &b, e, 32);
    assert(n == 1 && !e[0].down && e[0].code == 'A');

    /* Left Ctrl, then right Ctrl too: one VK_CONTROL press; releasing the
     * left one keeps it held, releasing both releases it. */
    memset(&a, 0, sizeof(a)); b = a; b.modifiers = 0x01;
    n = pw_hid_keyboard_events(&a, &b, e, 32);
    assert(n == 1 && e[0].down && e[0].code == 0x11);
    a = b; b.modifiers = 0x11;
    assert(pw_hid_keyboard_events(&a, &b, e, 32) == 0);
    a = b; b.modifiers = 0x10;
    assert(pw_hid_keyboard_events(&a, &b, e, 32) == 0);
    a = b; b.modifiers = 0;
    n = pw_hid_keyboard_events(&a, &b, e, 32);
    assert(n == 1 && !e[0].down && e[0].code == 0x11);
    /* The Windows keys are separate keys. */
    a = b; b.modifiers = 0x88;
    n = pw_hid_keyboard_events(&a, &b, e, 32);
    assert(n == 2 && e[0].code == 0x5b && e[1].code == 0x5c && e[0].down && e[1].down);
    a = b; b.modifiers = 0x80;
    n = pw_hid_keyboard_events(&a, &b, e, 32);
    assert(n == 1 && !e[0].down && e[0].code == 0x5b);

    /* Releases come before presses; error usages (1..3) and unmapped ones
     * give nothing; the output is bounded by its capacity. */
    memset(&a, 0, sizeof(a)); b = a;
    a.keys[0] = 0x04; b.keys[0] = 0x05; b.keys[1] = 0x01; b.keys[2] = 0xe8;
    n = pw_hid_keyboard_events(&a, &b, e, 32);
    assert(n == 2 && !e[0].down && e[0].code == 'A' && e[1].down && e[1].code == 'B');
    memset(&a, 0, sizeof(a));
    for (int i = 0; i < 6; i++) b.keys[i] = (uint8_t)(0x04 + i);
    b.modifiers = 0xff;
    assert(pw_hid_keyboard_events(&a, &b, e, 3) == 3);
}

static void test_mouse_buttons(void)
{
    PwHidEvent e[4];

    assert(pw_hid_mouse_button_events(0, PW_HID_MOUSE_LEFT, e, 4) == 1 && e[0].kind == PW_HID_EVENT_BUTTON &&
           e[0].code == 0 && e[0].down);
    assert(pw_hid_mouse_button_events(PW_HID_MOUSE_LEFT, PW_HID_MOUSE_RIGHT | PW_HID_MOUSE_MIDDLE, e, 4) == 3);
    assert(!e[0].down && e[0].code == 0 && e[1].down && e[1].code == 1 && e[2].down && e[2].code == 2);
    assert(pw_hid_mouse_button_events(7, 7, e, 4) == 0);
}

int main(void)
{
    test_usages();
    test_keyboard();
    test_mouse_buttons();
    printf("usb keyboard and mouse passed: usages to virtual keys, key and modifier changes, buttons\n");
    return 0;
}
