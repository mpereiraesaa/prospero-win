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
                           size_t capacity, PwGdiTargetView *view)
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

void pw_wine_pointer_init(PwWinePointer *pointer, uint32_t width, uint32_t height)
{
    if (!pointer) return;
    pointer->width = width ? width : 1u;
    pointer->height = height ? height : 1u;
    pointer->x = (int64_t)(pointer->width / 2u) << 16;
    pointer->y = (int64_t)(pointer->height / 2u) << 16;
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

static int64_t clamp(int64_t value, uint32_t size)
{
    int64_t last = (int64_t)(size - 1u) << 16;
    return value < 0 ? 0 : value > last ? last : value;
}

int pw_wine_pointer_step(PwWinePointer *pointer, uint8_t stick_x, uint8_t stick_y,
                         uint32_t speed, uint32_t elapsed_us, PwWineInput *out)
{
    int64_t x, y;

    if (!pointer || !out) return 0;
    x = clamp(pointer->x + travel(stick_x, speed, elapsed_us), pointer->width);
    y = clamp(pointer->y + travel(stick_y, speed, elapsed_us), pointer->height);
    int moved = (x >> 16) != (pointer->x >> 16) || (y >> 16) != (pointer->y >> 16);
    pointer->x = x;
    pointer->y = y;
    if (!moved) return 0;
    *out = (PwWineInput){ PW_WINE_INPUT_MOUSE_MOVE, 0, (int32_t)(x >> 16), (int32_t)(y >> 16), 0 };
    return 1;
}
