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

static size_t add_edges(const PwPad *pad, uint32_t edges, uint32_t down, PwWineInput *out,
                        size_t used, size_t max)
{
    for (size_t i = 0; i < pad->map_count && used < max; i++) {
        if (!(edges & pad->map[i].mask)) continue;
        out[used].type = PW_WINE_INPUT_KEY;
        out[used].code = pad->map[i].virtual_key;
        out[used].x = out[used].y = 0;
        out[used].down = down;
        used++;
    }
    return used;
}

size_t pw_wine_pad_inputs(const PwPad *pad, PwWineInput *out, size_t max)
{
    size_t used;

    if (!pad || !out || !pad->map) return 0;
    used = add_edges(pad, pad->released_edges, 0, out, 0, max);
    return add_edges(pad, pad->pressed_edges, 1, out, used, max);
}
