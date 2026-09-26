/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../native/pw_wine_display.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

enum { LEFT = 0x400u, RIGHT = 0x800u, CROSS = 0x4000u, UNMAPPED = 0x10u };

static const PwPadKeyMap map[] = {
    { LEFT, 'Z', 0, 0, "left-flipper" },
    { RIGHT, 0xbf, 0, 0, "right-flipper" },
    { CROSS, 0x20, 0, 0, "plunger" },
};

static void test_frames(void)
{
    PwWineFrameBox box;
    PwGdiTargetView view;
    uint8_t source[3 * 16], out[64], small[8], storage[48];
    uint64_t seen = 0;

    for (size_t i = 0; i < sizeof(source); i++) source[i] = (uint8_t)i;
    assert(pw_wine_frame_box_init(NULL, storage, 48) == -1);
    assert(pw_wine_frame_box_init(&box, NULL, 48) == -1 && pw_wine_frame_box_init(&box, storage, 0) == -1);
    assert(!pw_wine_frame_box_init(&box, storage, sizeof(storage)));
    assert(pw_wine_frame_box_take(&box, &seen, out, sizeof(out), &view) == 0);

    /* Rows are packed: a 3x2 frame with a 16-byte stride keeps 12 bytes a row. */
    assert(!pw_wine_frame_box_put(&box, source, 3, 2, 16));
    assert(pw_wine_frame_box_take(&box, &seen, small, sizeof(small), &view) == -1 && seen == 0);
    assert(pw_wine_frame_box_take(&box, &seen, out, sizeof(out), &view) == 1 && seen == 1);
    assert(view.pixels == out && view.width == 3 && view.height == 2 && view.stride == 12 &&
           view.bytes == 24);
    assert(!memcmp(out, source, 12) && !memcmp(out + 12, source + 16, 12));
    assert(pw_wine_frame_box_take(&box, &seen, out, sizeof(out), &view) == 0);

    /* Only the newest of several frames is taken. */
    assert(!pw_wine_frame_box_put(&box, source, 1, 1, 4));
    assert(!pw_wine_frame_box_put(&box, source + 4, 2, 1, 8));
    assert(pw_wine_frame_box_take(&box, &seen, out, sizeof(out), &view) == 1 && seen == 3);
    assert(view.width == 2 && view.height == 1 && !memcmp(out, source + 4, 8));

    /* Refusals: invalid, stride below a row, larger than the box. */
    assert(pw_wine_frame_box_put(&box, NULL, 1, 1, 4) == -1);
    assert(pw_wine_frame_box_put(&box, source, 0, 1, 4) == -1);
    assert(pw_wine_frame_box_put(&box, source, 1, 0, 4) == -1);
    assert(pw_wine_frame_box_put(&box, source, 2, 1, 4) == -1);
    assert(pw_wine_frame_box_put(&box, source, 4, 4, 16) == -1);
    assert(pw_wine_frame_box_put(&box, source, 0x40000000u, 2, 0xffffffffu) == -1);
    assert(box.rejected == 6 && box.sequence == 3);
    assert(pw_wine_frame_box_put(NULL, source, 1, 1, 4) == -1);
    assert(pw_wine_frame_box_take(&box, NULL, out, sizeof(out), &view) == -1);
    assert(pw_wine_frame_box_take(&box, &seen, NULL, sizeof(out), &view) == -1);
    assert(pw_wine_frame_box_take(&box, &seen, out, sizeof(out), NULL) == -1);
    pw_wine_frame_box_destroy(&box);
    pw_wine_frame_box_destroy(&box);              /* a second destroy is harmless */
    pw_wine_frame_box_destroy(NULL);
    assert(pw_wine_frame_box_put(&box, source, 1, 1, 4) == -1);
    assert(pw_wine_frame_box_take(&box, &seen, out, sizeof(out), &view) == -1);
}

/* Wine's threads put while the title's thread takes. */
static PwWineFrameBox shared;
static uint8_t shared_storage[16];

static void *producer(void *arg)
{
    uint32_t pixels[4];

    (void)arg;
    for (uint32_t i = 1; i <= 2000; i++) {
        for (int p = 0; p < 4; p++) pixels[p] = i;
        assert(!pw_wine_frame_box_put(&shared, pixels, 2, 2, 8));
    }
    return NULL;
}

static void test_threads(void)
{
    pthread_t thread;
    PwGdiTargetView view;
    uint32_t out[4], last = 0;
    uint64_t seen = 0;

    assert(!pw_wine_frame_box_init(&shared, shared_storage, sizeof(shared_storage)));
    assert(!pthread_create(&thread, NULL, producer, NULL));
    while (last < 2000) {
        if (pw_wine_frame_box_take(&shared, &seen, (uint8_t *)out, sizeof(out), &view) != 1)
            continue;
        /* never a torn frame, never older than the last one taken */
        assert(out[0] == out[1] && out[1] == out[2] && out[2] == out[3] && out[0] >= last);
        last = out[0];
    }
    assert(!pthread_join(thread, NULL) && seen == 2000);
    pw_wine_frame_box_destroy(&shared);
}

static void test_pad(void)
{
    PwPad pad;
    PwWineInput events[8];
    PwPadSample sample = { .connected = 1 };

    assert(!pw_pad_init(&pad, map, sizeof(map) / sizeof(map[0])));
    sample.buttons = LEFT | CROSS | UNMAPPED;
    assert(!pw_pad_track(&pad, &sample, 1));
    assert(pw_wine_pad_inputs(&pad, events, 8) == 2);
    assert(events[0].type == PW_WINE_INPUT_KEY && events[0].code == 'Z' && events[0].down == 1);
    assert(events[1].code == 0x20 && events[1].down == 1 && !events[1].x && !events[1].y);

    /* Releases come before presses; unmapped buttons send none. */
    sample.buttons = RIGHT | UNMAPPED;
    assert(!pw_pad_track(&pad, &sample, 1));
    assert(pw_wine_pad_inputs(&pad, events, 8) == 3);
    assert(events[0].code == 'Z' && events[0].down == 0);
    assert(events[1].code == 0x20 && events[1].down == 0);
    assert(events[2].code == 0xbf && events[2].down == 1);
    assert(pw_wine_pad_inputs(&pad, events, 1) == 1 && events[0].code == 'Z');

    /* An unchanged batch sends nothing; a disconnection releases what is held. */
    assert(!pw_pad_track(&pad, &sample, 1));
    assert(pw_wine_pad_inputs(&pad, events, 8) == 0);
    sample.connected = 0;
    assert(!pw_pad_track(&pad, &sample, 1));
    assert(pw_wine_pad_inputs(&pad, events, 8) == 1 && events[0].code == 0xbf && !events[0].down);

    assert(pw_wine_pad_inputs(NULL, events, 8) == 0 && pw_wine_pad_inputs(&pad, NULL, 8) == 0);
    pad.map = NULL;
    assert(pw_wine_pad_inputs(&pad, events, 8) == 0);
}

int main(void)
{
    test_frames();
    test_threads();
    test_pad();
    printf("wine display passed: frame box copy, newest frame, refusals, concurrent put/take, "
           "pad edges to key events\n");
    return 0;
}
