/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_wine_display.h"
#include <string.h>

int pw_wine_frame_box_init(PwWineFrameBox *box, uint8_t *storage, size_t bytes)
{
    if (!box || !storage || !bytes) return -1;
    memset(box, 0, sizeof(*box));
    if (pthread_mutex_init(&box->lock, NULL)) return -1;
    box->pixels = storage;
    box->capacity = bytes;
    return 0;
}

void pw_wine_frame_box_destroy(PwWineFrameBox *box)
{
    if (!box || !box->pixels) return;
    pthread_mutex_destroy(&box->lock);
    box->pixels = NULL;
}

int pw_wine_frame_box_put(PwWineFrameBox *box, const void *bgra, uint32_t width,
                          uint32_t height, uint32_t stride)
{
    size_t row, bytes;

    if (!box || !box->pixels) return -1;
    row = (size_t)width * 4u;
    bytes = row * height;
    pthread_mutex_lock(&box->lock);
    if (!bgra || !width || !height || stride < row || bytes / height != row ||
        bytes > box->capacity) {
        box->rejected++;
        pthread_mutex_unlock(&box->lock);
        return -1;
    }
    for (uint32_t y = 0; y < height; y++)
        memcpy(box->pixels + y * row, (const uint8_t *)bgra + (size_t)y * stride, row);
    box->width = width;
    box->height = height;
    box->sequence++;
    pthread_mutex_unlock(&box->lock);
    return 0;
}

int pw_wine_frame_box_take(PwWineFrameBox *box, uint64_t *seen, uint8_t *out,
                           size_t capacity, PwPresentView *view)
{
    size_t bytes;
    int status = 0;

    if (!box || !box->pixels || !seen || !out || !view) return -1;
    pthread_mutex_lock(&box->lock);
    if (box->sequence != *seen) {
        bytes = (size_t)box->width * 4u * box->height;
        if (bytes > capacity) status = -1;
        else {
            memcpy(out, box->pixels, bytes);
            view->pixels = out;
            view->width = box->width;
            view->height = box->height;
            view->stride = box->width * 4u;
            view->bytes = (uint32_t)bytes;
            *seen = box->sequence;
            status = 1;
        }
    }
    pthread_mutex_unlock(&box->lock);
    return status;
}

static size_t add_edges(const PwGameInput *input, uint32_t edges, uint32_t down,
                        PwWineInput *out, size_t used, size_t max)
{
    for (size_t i = 0; i < PW_GAME_BUTTON_COUNT && used < max; i++) {
        const PwGameBinding *b = &input->bindings[i];
        if (!(edges & b->mask) || (b->kind != PW_GAME_BIND_KEY && b->kind != PW_GAME_BIND_MOUSE))
            continue;
        out[used].type = b->kind == PW_GAME_BIND_KEY ? PW_WINE_INPUT_KEY : PW_WINE_INPUT_MOUSE_BUTTON;
        out[used].code = b->code;
        out[used].x = out[used].y = 0;
        out[used].down = down;
        used++;
    }
    return used;
}

size_t pw_wine_game_inputs(const PwGameInput *input, uint32_t pressed, uint32_t released,
                           PwWineInput *out, size_t max)
{
    size_t used;

    if (!input || !out) return 0;
    used = add_edges(input, released, 0, out, 0, max);
    return add_edges(input, pressed, 1, out, used, max);
}

static const struct { uint32_t dualsense; uint16_t xinput; } pad_buttons[] = {
    { 0x4000u, 0x1000u }, { 0x2000u, 0x2000u }, { 0x8000u, 0x4000u }, { 0x1000u, 0x8000u },
    { 0x400u, 0x0100u }, { 0x800u, 0x0200u }, { 0x2u, 0x0040u }, { 0x4u, 0x0080u },
    { 0x10u, 0x0001u }, { 0x40u, 0x0002u }, { 0x80u, 0x0004u }, { 0x20u, 0x0008u },
    { 0x8u, 0x0010u }, { 0x1u, 0x0020u }, { 0x100000u, 0x0400u },
};

/* One stick axis (0..255, 0x80 centred) as XInput's -32768..32767, centre
 * 0; up is 0 on the DualSense and positive in XInput. */
static int16_t pad_axis(uint8_t raw, int up)
{
    int32_t d = up ? 0x80 - (int32_t)raw : (int32_t)raw - 0x80;   /* -127..128 or -128..127 */
    int32_t reach = d < 0 ? (up ? 127 : 128) : (up ? 128 : 127);
    return (int16_t)(d * (d < 0 ? 32768 : 32767) / reach);
}

int pw_wine_game_pad(const PwPadPs5 *pad, PwWinePad *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!pad || !pad->core.connected) return 0;
    out->connected = 1;
    for (size_t i = 0; i < sizeof(pad_buttons) / sizeof(pad_buttons[0]); i++)
        if (pad->core.previous_buttons & pad_buttons[i].dualsense) out->buttons |= pad_buttons[i].xinput;
    out->left_trigger = pad->l2;
    out->right_trigger = pad->r2;
    out->thumb_lx = pad_axis(pad->left_stick.x, 0);
    out->thumb_ly = pad_axis(pad->left_stick.y, 1);
    out->thumb_rx = pad_axis(pad->right_stick.x, 0);
    out->thumb_ry = pad_axis(pad->right_stick.y, 1);
    return 1;
}

int pw_wine_script_pad(PwWinePad *pad, int connected, const PwScriptPad *script, int scripted)
{
    if (!pad) return 0;
    if (!connected) memset(pad, 0, sizeof(*pad));
    if (!script || !scripted) return connected ? 1 : 0;
    pad->connected = 1;
    pad->buttons |= script->buttons;
    if (script->stick[0][0] || script->stick[0][1]) {
        pad->thumb_lx = script->stick[0][0];
        pad->thumb_ly = script->stick[0][1];
    }
    if (script->stick[1][0] || script->stick[1][1]) {
        pad->thumb_rx = script->stick[1][0];
        pad->thumb_ry = script->stick[1][1];
    }
    return 1;
}

/* Signed 1/65536-pixel travel of one axis. */
static int64_t travel(uint8_t stick, uint32_t speed, uint32_t elapsed_us)
{
    int32_t axis = (int32_t)stick - 0x80, magnitude = axis < 0 ? -axis : axis;
    int64_t t, v;

    if (magnitude <= PW_WINE_POINTER_DEADZONE) return 0;
    t = (int64_t)(magnitude - PW_WINE_POINTER_DEADZONE) * 65536 / (127 - PW_WINE_POINTER_DEADZONE);
    if (t > 65536) t = 65536;
    v = t * t >> 16;                       /* 0..65536: a squared curve */
    v = v * speed * elapsed_us / 1000000;   /* < 2^63 for speed <= 20000 */
    return axis < 0 ? -v : v;
}

int pw_wine_pointer_step(PwWinePointer *pointer, uint8_t stick_x, uint8_t stick_y,
                         uint32_t speed, uint32_t elapsed_us, PwWineInput *out)
{
    int64_t dx, dy;

    if (!pointer || !out) return 0;
    pointer->x += travel(stick_x, speed, elapsed_us);
    pointer->y += travel(stick_y, speed, elapsed_us);
    /* whole pixels, towards zero, so both directions keep their fraction */
    dx = pointer->x / 65536;
    dy = pointer->y / 65536;
    if (!dx && !dy) return 0;
    pointer->x -= dx * 65536;
    pointer->y -= dy * 65536;
    *out = (PwWineInput){ PW_WINE_INPUT_MOUSE_MOVE, 0, (int32_t)dx, (int32_t)dy, 0 };
    return 1;
}
