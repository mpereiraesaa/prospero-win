/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../native/pw_hid_ps5.h"
#include "../include/prospero_win.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

/* A fake system: which modules load, the open results, and the reads. */
static int keyboard_loads, mouse_loads, merged_refused, lines;
static int32_t opened_user, merged_flags[2];
static int merged_calls;
static uint8_t keyboard_state[PW_HID_PS5_KEYBOARD_STATE];
static uint8_t mouse_records[4 * PW_HID_PS5_MOUSE_RECORD];
static int mouse_count;

static int32_t fake_load(const char *path)
{
    if (!strcmp(path, "libSceKeyboard")) return keyboard_loads ? 7 : -1;
    if (!strcmp(path, "libSceMouse")) return mouse_loads ? 8 : -1;
    return -1;
}
static int fake_init(void) { return 0; }
static int fake_keyboard_open(int32_t user, int32_t type, int32_t index, void *param)
{
    (void)type; (void)index; (void)param;
    opened_user = user;
    return 3;
}
static int fake_mouse_open(int32_t user, int32_t type, int32_t index, void *param)
{
    (void)user; (void)type; (void)index;
    merged_flags[merged_calls++ & 1] = ((uint8_t *)param)[0];
    return merged_refused && ((uint8_t *)param)[0] ? -5 : 4;
}
static int fake_keyboard_read(int32_t handle, void *state)
{
    assert(handle == 3);
    memcpy(state, keyboard_state, sizeof(keyboard_state));
    return 0;
}
static int fake_mouse_read(int32_t handle, void *records, int32_t count)
{
    assert(handle == 4 && count == PW_HID_PS5_MOUSE_RECORDS);
    memcpy(records, mouse_records, (size_t)mouse_count * PW_HID_PS5_MOUSE_RECORD);
    return mouse_count;
}
static int fake_resolve(int32_t module, const char *name, void **address)
{
    static const struct { const char *name; void *address; } table[] = {
        { "sceKeyboardInit", (void *)fake_init }, { "sceKeyboardOpen", (void *)fake_keyboard_open },
        { "sceKeyboardReadState", (void *)fake_keyboard_read },
        { "sceMouseInit", (void *)fake_init }, { "sceMouseOpen", (void *)fake_mouse_open },
        { "sceMouseRead", (void *)fake_mouse_read },
    };
    (void)module;
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (!strcmp(table[i].name, name)) { *address = table[i].address; return 0; }
    return -1;
}
static void fake_log(const char *line) { (void)line; lines++; }

static const PwHidPs5Ops ops = { fake_load, fake_resolve, fake_log };

static void keyboard(uint32_t modifiers, const uint16_t *usages, int count)
{
    memset(keyboard_state, 0, sizeof(keyboard_state));
    keyboard_state[16] = 1;
    put32(keyboard_state + 20, (uint32_t)count);
    put32(keyboard_state + 28, modifiers);
    if (count) memcpy(keyboard_state + 32, usages, (size_t)count * 2);
}

static void mouse(int i, uint64_t time, uint32_t buttons, int32_t x, int32_t y, int32_t wheel)
{
    uint8_t *r = mouse_records + i * PW_HID_PS5_MOUSE_RECORD;
    memset(r, 0, PW_HID_PS5_MOUSE_RECORD);
    put64(r, time);
    r[8] = 1;
    put32(r + 12, buttons);
    put32(r + 16, (uint32_t)x);
    put32(r + 20, (uint32_t)y);
    put32(r + 24, (uint32_t)wheel);
}

static void test_keyboard_layout(void)
{
    static const uint16_t usages[18] = { 0x04, 0x105, 0x06 };
    PwHidKeyboard k;

    keyboard(0x12, usages, 3);
    assert(pw_hid_ps5_keyboard_state(keyboard_state, &k) == 1);
    assert(k.modifiers == 0x12 && k.keys[0] == 0x04 && k.keys[1] == 0 && k.keys[2] == 0x06 && !k.keys[3]);
    /* A count beyond the report is bounded; a disconnected keyboard is empty. */
    keyboard(0, usages, 3);
    put32(keyboard_state + 20, 1000);
    assert(pw_hid_ps5_keyboard_state(keyboard_state, &k) == 1 && k.keys[0] == 0x04);
    keyboard_state[16] = 0;
    assert(pw_hid_ps5_keyboard_state(keyboard_state, &k) == 0 && !k.keys[0] && !k.modifiers);
}

static void test_mouse_layout(void)
{
    int32_t dx = 0, dy = 0, wheel = 0;
    uint32_t buttons = 0;
    uint64_t newest = 0;

    /* Motion sums; the buttons are those of the newest record, whatever
     * the order; the "intercepted" high bit is dropped. */
    mouse(0, 20, 0x80000001u, 3, -4, 1);
    mouse(1, 10, 0x2, 5, 6, 0);
    mouse(2, 30, 0, 0, 0, 0);
    mouse_records[2 * PW_HID_PS5_MOUSE_RECORD + 8] = 0;   /* disconnected */
    assert(pw_hid_ps5_mouse_records(mouse_records, 3, &dx, &dy, &wheel, &buttons, &newest) == 2);
    assert(dx == 8 && dy == 2 && wheel == 1 && buttons == PW_HID_MOUSE_LEFT && newest == 20);
}

static void test_open_and_poll(void)
{
    static const uint16_t w[1] = { 0x1a };
    PwHidPs5 hid;
    PwHidPs5Poll poll;

    keyboard_loads = mouse_loads = 0;
    assert(pw_hid_ps5_open(&hid, &ops, 1000) == PW_ERR_NOT_FOUND && lines == 2);
    pw_hid_ps5_poll(&hid, &poll);
    assert(poll.count == 0 && !poll.dx);
    assert(pw_hid_ps5_retry(&hid) == PW_ERR_NOT_FOUND && lines == 2);   /* quiet */

    /* The keyboard plugged in later is opened by a retry. */
    keyboard_loads = 1;
    assert(pw_hid_ps5_retry(&hid) == PW_OK && hid.keyboard == 3 && hid.mouse < 0 && lines == 3);

    /* The merged mouse handle refused: one per device. */
    keyboard_loads = mouse_loads = merged_refused = 1;
    merged_calls = 0;
    assert(pw_hid_ps5_open(&hid, &ops, 1000) == PW_OK && opened_user == 1000);
    assert(hid.keyboard == 3 && hid.mouse == 4 && merged_calls == 2 && merged_flags[0] == 1 && !merged_flags[1]);

    /* Ctrl+W pressed with a click and motion, then everything released. */
    keyboard(0x01, w, 1);
    mouse(0, 1, PW_HID_MOUSE_LEFT, 7, -2, 0);
    mouse_count = 1;
    lines = 0;
    pw_hid_ps5_poll(&hid, &poll);
    assert(poll.count == 3 && poll.dx == 7 && poll.dy == -2);
    assert(poll.events[0].kind == PW_HID_EVENT_KEY && poll.events[0].code == 0x11 && poll.events[0].down);
    assert(poll.events[1].code == 'W' && poll.events[1].down);
    assert(poll.events[2].kind == PW_HID_EVENT_BUTTON && poll.events[2].code == 0 && poll.events[2].down);
    assert(lines == 2);                                   /* the first data is dumped */

    keyboard(0, NULL, 0);
    mouse(0, 2, 0, 0, 0, 0);
    pw_hid_ps5_poll(&hid, &poll);
    assert(poll.count == 3 && !poll.events[0].down && poll.events[0].code == 'W' &&
           poll.events[1].code == 0x11 && !poll.events[2].down);
    assert(lines == 3);                                   /* an empty keyboard is not dumped */

    /* No new records: nothing changes. */
    mouse_count = 0;
    pw_hid_ps5_poll(&hid, &poll);
    assert(poll.count == 0 && !poll.dx && !poll.dy);
}

int main(void)
{
    test_keyboard_layout();
    test_mouse_layout();
    test_open_and_poll();
    puts("ps5 usb keyboard and mouse passed");
    return 0;
}
