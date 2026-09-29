/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_hid_ps5.h"
#include "../include/prospero_win.h"
#include <stdio.h>
#include <string.h>

static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static int32_t s32(const uint8_t *p) { int32_t v; memcpy(&v, p, 4); return v; }
static uint64_t u64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

int pw_hid_ps5_keyboard_state(const uint8_t *state, PwHidKeyboard *out)
{
    int32_t keys;

    memset(out, 0, sizeof(*out));
    if (!state[16]) return 0;                                  /* connected */
    keys = s32(state + 20);
    if (keys < 0) keys = 0;
    if (keys > PW_HID_KEYS) keys = PW_HID_KEYS;
    out->modifiers = (uint8_t)u32(state + 28);
    for (int32_t i = 0; i < keys; i++) {
        uint16_t usage;
        memcpy(&usage, state + 32 + 2 * i, 2);
        out->keys[i] = usage <= 0xff ? (uint8_t)usage : 0;
    }
    return 1;
}

int pw_hid_ps5_mouse_records(const uint8_t *records, int count, int32_t *dx, int32_t *dy,
                             int32_t *wheel, uint32_t *buttons, uint64_t *newest)
{
    int used = 0;

    /* The order of the records is not established: the motion is their sum,
     * the buttons those of the newest. */
    for (int i = 0; i < count; i++) {
        const uint8_t *r = records + (size_t)i * PW_HID_PS5_MOUSE_RECORD;
        if (!r[8]) continue;                                   /* connected */
        *dx += s32(r + 16);
        *dy += s32(r + 20);
        *wheel += s32(r + 24);
        if (u64(r) >= *newest) {
            *newest = u64(r);
            *buttons = u32(r + 12) & (PW_HID_MOUSE_LEFT | PW_HID_MOUSE_RIGHT | PW_HID_MOUSE_MIDDLE);
        }
        used++;
    }
    return used;
}

static void logf_(const PwHidPs5 *hid, const char *text)
{
    if (hid->ops.log) hid->ops.log(text);
}

static void dump(const PwHidPs5 *hid, const char *what, const uint8_t *bytes, size_t length)
{
    char line[16 + 2 * PW_HID_PS5_KEYBOARD_STATE];
    size_t at = (size_t)snprintf(line, sizeof(line), "%s ", what);

    for (size_t i = 0; i < length && at + 3 < sizeof(line); i++)
        at += (size_t)snprintf(line + at, sizeof(line) - at, "%02x", bytes[i]);
    logf_(hid, line);
}

/* Opens the keyboard or the mouse when it is not open yet. A failure is
 * logged once, so a retry every few seconds stays quiet. */
static void open_keyboard(PwHidPs5 *hid)
{
    int (*init)(void) = NULL;
    int (*open)(int32_t, int32_t, int32_t, void *) = NULL;
    uint8_t param[8] = { 0 };
    char line[160];
    int32_t module = hid->ops.load_module("libSceKeyboard");
    int handle = -1, init_rc = -1;

    if (module >= 0 && !hid->ops.resolve(module, "sceKeyboardInit", (void **)&init) &&
        !hid->ops.resolve(module, "sceKeyboardOpen", (void **)&open) &&
        !hid->ops.resolve(module, "sceKeyboardReadState", (void **)&hid->keyboard_read_state)) {
        init_rc = init();
        handle = open(hid->user_id, 0, 0, param);
    }
    if (handle >= 0) hid->keyboard = handle;
    if (handle >= 0 || !hid->keyboard_failed) {
        snprintf(line, sizeof(line), "keyboard module=%#x init=%#x open=%#x user=%d", (unsigned)module,
                 (unsigned)init_rc, (unsigned)handle, (int)hid->user_id);
        logf_(hid, line);
    }
    hid->keyboard_failed = handle < 0;
}

static void open_mouse(PwHidPs5 *hid)
{
    int (*init)(void) = NULL;
    int (*open)(int32_t, int32_t, int32_t, void *) = NULL;
    uint8_t param[8] = { 0 };
    char line[160];
    int32_t module = hid->ops.load_module("libSceMouse");
    int handle = -1, init_rc = -1;

    if (module >= 0 && !hid->ops.resolve(module, "sceMouseInit", (void **)&init) &&
        !hid->ops.resolve(module, "sceMouseOpen", (void **)&open) &&
        !hid->ops.resolve(module, "sceMouseRead", (void **)&hid->mouse_read)) {
        init_rc = init();
        /* Every mouse through one handle, as the PS5 ports that ran on
         * hardware open it; one per device where the system refuses that. */
        param[0] = 1;
        handle = open(hid->user_id, 0, 0, param);
        if (handle < 0) {
            param[0] = 0;
            handle = open(hid->user_id, 0, 0, param);
        }
    }
    if (handle >= 0) hid->mouse = handle;
    if (handle >= 0 || !hid->mouse_failed) {
        snprintf(line, sizeof(line), "mouse module=%#x init=%#x open=%#x merged=%u user=%d",
                 (unsigned)module, (unsigned)init_rc, (unsigned)handle, (unsigned)param[0],
                 (int)hid->user_id);
        logf_(hid, line);
    }
    hid->mouse_failed = handle < 0;
}

int pw_hid_ps5_open(PwHidPs5 *hid, const PwHidPs5Ops *ops, int32_t user_id)
{
    if (!hid || !ops) return PW_ERR_PRECONDITION;
    memset(hid, 0, sizeof(*hid));
    hid->ops = *ops;
    hid->user_id = user_id;
    hid->keyboard = hid->mouse = -1;
    return pw_hid_ps5_retry(hid);
}

int pw_hid_ps5_retry(PwHidPs5 *hid)
{
    if (!hid || !hid->ops.load_module) return PW_ERR_PRECONDITION;
    if (hid->keyboard < 0) open_keyboard(hid);
    if (hid->mouse < 0) open_mouse(hid);
    return hid->keyboard >= 0 || hid->mouse >= 0 ? PW_OK : PW_ERR_NOT_FOUND;
}

void pw_hid_ps5_poll(PwHidPs5 *hid, PwHidPs5Poll *poll)
{
    memset(poll, 0, sizeof(*poll));
    if (!hid) return;
    if (hid->keyboard >= 0) {
        uint8_t state[PW_HID_PS5_KEYBOARD_STATE] = { 0 };
        PwHidKeyboard now;

        if (hid->keyboard_read_state(hid->keyboard, state) == 0) {
            pw_hid_ps5_keyboard_state(state, &now);
            if (hid->keyboard_dumps < PW_HID_PS5_DUMPS && (now.keys[0] || now.modifiers)) {
                dump(hid, "keyboard state", state, sizeof(state));
                hid->keyboard_dumps++;
            }
            poll->count += pw_hid_keyboard_events(&hid->keys, &now, poll->events + poll->count,
                                                  PW_HID_PS5_EVENTS - poll->count);
            hid->keys = now;
        }
    }
    if (hid->mouse >= 0) {
        uint8_t records[PW_HID_PS5_MOUSE_RECORDS * PW_HID_PS5_MOUSE_RECORD];
        int got = hid->mouse_read(hid->mouse, records, PW_HID_PS5_MOUSE_RECORDS);

        if (got > 0) {
            uint32_t buttons = hid->buttons;
            if (got > PW_HID_PS5_MOUSE_RECORDS) got = PW_HID_PS5_MOUSE_RECORDS;
            for (int i = 0; i < got && hid->mouse_dumps < PW_HID_PS5_DUMPS; i++, hid->mouse_dumps++)
                dump(hid, "mouse record", records + (size_t)i * PW_HID_PS5_MOUSE_RECORD, PW_HID_PS5_MOUSE_RECORD);
            pw_hid_ps5_mouse_records(records, got, &poll->dx, &poll->dy, &poll->wheel, &buttons,
                                     &hid->mouse_time);
            poll->count += pw_hid_mouse_button_events(hid->buttons, buttons, poll->events + poll->count,
                                                      PW_HID_PS5_EVENTS - poll->count);
            hid->buttons = buttons;
        }
    }
}

#ifndef PW_HID_PS5_HOST_TEST
int sceSysmoduleLoadModule(uint16_t id);
int sceSysmoduleGetModuleHandleInternal(uint32_t id, int32_t *handle);
int sceKernelDlsym(int32_t handle, const char *symbol, void **address);

/* A fake-signed title may not load these by path (0x80020063,
 * ESDKVERSION, measured): the system loads them as its modules
 * (OpenOrbis's SCE_SYSMODULE_KEYBOARD and _MOUSE; ProsperoLight, a PS5
 * title, does so on hardware). */
static const struct { const char *name; uint16_t id; } sysmodules[] = {
    { "libSceKeyboard", 0x0106 }, { "libSceMouse", 0x00a9 },
};

/* The system's handle for a module it loaded, by its id (measured on the
 * console: the kernel's module list shows only the title's own), or its
 * internal id (0x80000000 | id). */
static int32_t sysmodule_handle(uint16_t id)
{
    int32_t handle = -1;

    if (sceSysmoduleGetModuleHandleInternal(id, &handle) == 0 && handle >= 0) return handle;
    handle = -1;
    if (sceSysmoduleGetModuleHandleInternal(0x80000000u | id, &handle) == 0 && handle >= 0) return handle;
    return -1;
}

static int32_t load(const char *name)
{
    for (size_t i = 0; i < sizeof(sysmodules) / sizeof(sysmodules[0]); i++) {
        if (strcmp(sysmodules[i].name, name)) continue;
        int status = sceSysmoduleLoadModule(sysmodules[i].id);
        int32_t handle = sysmodule_handle(sysmodules[i].id);
        return handle >= 0 ? handle : status < 0 ? status : -1;
    }
    return -1;
}

void pw_hid_ps5_preload(void (*log)(const char *line))
{
    char line[96];

    for (size_t i = 0; i < sizeof(sysmodules) / sizeof(sysmodules[0]); i++) {
        int status = sceSysmoduleLoadModule(sysmodules[i].id);
        snprintf(line, sizeof(line), "preload %s id=%#x status=%#x handle=%d", sysmodules[i].name,
                 (unsigned)sysmodules[i].id, (unsigned)status, (int)sysmodule_handle(sysmodules[i].id));
        if (log) log(line);
    }
}

const PwHidPs5Ops *pw_hid_ps5_system_ops(void (*log)(const char *line))
{
    static PwHidPs5Ops ops;
    ops.load_module = load;
    ops.resolve = sceKernelDlsym;
    ops.log = log;
    return &ops;
}
#endif
