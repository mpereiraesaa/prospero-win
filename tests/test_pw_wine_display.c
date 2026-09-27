/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../native/pw_wine_display.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

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

static const char preset_text[] =
    "[input]\nl1 = z\nr1 = slash\ncross = space\nr2 = mouse_left\nl2 = mouse_right\n"
    "square = none\n";
enum { L1 = 0x400u, R1 = 0x800u, CROSS = 0x4000u, SQUARE = 0x8000u, L2 = 0x100u, R2 = 0x200u,
       CIRCLE = 0x2000u };

static void test_inputs(void)
{
    PwGameInput input;
    PwWineInput events[8];

    pw_game_input_init(&input);
    assert(pw_game_input_parse((const uint8_t *)preset_text, sizeof(preset_text) - 1, &input) == PW_OK);
    assert(pw_wine_game_inputs(&input, L1 | CROSS | CIRCLE | SQUARE, 0, events, 8) == 2);
    assert(events[0].type == PW_WINE_INPUT_KEY && events[0].code == 0x20 && events[0].down == 1);
    assert(events[1].code == 'Z' && events[1].down == 1 && !events[1].x && !events[1].y);

    /* Releases come first; mouse bindings send buttons; unbound and none send nothing. */
    assert(pw_wine_game_inputs(&input, R2 | L2, L1 | CROSS, events, 8) == 4);
    assert(events[0].code == 0x20 && !events[0].down && events[1].code == 'Z' && !events[1].down);
    assert(events[2].type == PW_WINE_INPUT_MOUSE_BUTTON && events[2].code == 1 && events[2].down);
    assert(events[3].type == PW_WINE_INPUT_MOUSE_BUTTON && events[3].code == 0 && events[3].down);
    assert(pw_wine_game_inputs(&input, R1 | L1, 0, events, 1) == 1 && events[0].code == 'Z');
    assert(pw_wine_game_inputs(&input, 0, 0, events, 8) == 0);
    assert(pw_wine_game_inputs(NULL, L1, 0, events, 8) == 0 && pw_wine_game_inputs(&input, L1, 0, NULL, 8) == 0);
}

static void test_pointer(void)
{
    PwWinePointer pointer;
    PwWineInput move;

    pw_wine_pointer_init(&pointer, 800, 600);
    assert(pointer.x >> 16 == 400 && pointer.y >> 16 == 300);
    /* At rest and inside the dead zone nothing moves. */
    assert(!pw_wine_pointer_step(&pointer, 0x80, 0x80, 1200, 16000, &move));
    assert(!pw_wine_pointer_step(&pointer, 0x80 + PW_WINE_POINTER_DEADZONE, 0x80 - PW_WINE_POINTER_DEADZONE,
                                 1200, 1000000, &move));
    /* Full tilt right for a second covers the speed; up moves y down to 0. */
    assert(pw_wine_pointer_step(&pointer, 0xff, 0x80, 300, 1000000, &move) == 1);
    assert(move.type == PW_WINE_INPUT_MOUSE_MOVE && move.x == 700 && move.y == 300);
    assert(pw_wine_pointer_step(&pointer, 0x80, 0x00, 1200, 1000000, &move) == 1);
    assert(move.x == 700 && move.y == 0);
    /* Held against the edges it stays on the desktop. */
    assert(pw_wine_pointer_step(&pointer, 0xff, 0xff, 20000, 1000000, &move) == 1);
    assert(move.x == 799 && move.y == 599);
    assert(!pw_wine_pointer_step(&pointer, 0xff, 0xff, 20000, 1000000, &move));
    assert(pw_wine_pointer_step(&pointer, 0x00, 0x00, 20000, 1000000, &move) == 1 && !move.x && !move.y);
    /* Half tilt is slower than half speed (squared curve), and small steps add up. */
    pw_wine_pointer_init(&pointer, 800, 600);
    assert(pw_wine_pointer_step(&pointer, 0x80 + 74, 0x80, 1000, 1000000, &move) == 1);
    assert(move.x > 400 + 200 && move.x < 400 + 300);
    pw_wine_pointer_init(&pointer, 800, 600);
    int moves = 0;
    for (int i = 0; i < 100; i++) moves += pw_wine_pointer_step(&pointer, 0xff, 0x80, 60, 16000, &move);
    assert(moves > 0 && pointer.x >> 16 >= 400 + 95 && pointer.x >> 16 <= 400 + 96);
    pw_wine_pointer_init(&pointer, 0, 0);
    assert(pointer.width == 1 && !pw_wine_pointer_step(&pointer, 0xff, 0xff, 1000, 1000, &move));
    assert(!pw_wine_pointer_step(NULL, 0xff, 0x80, 1, 1, &move) && !pw_wine_pointer_step(&pointer, 0, 0, 1, 1, NULL));
    pw_wine_pointer_init(NULL, 1, 1);
}

static void test_pointer_resize(void)
{
    PwWinePointer pointer;
    PwWineInput move;

    /* A menu widens the frame: the pointer stays where it was, to the pixel
     * and to the fraction of one. */
    pw_wine_pointer_init(&pointer, 160, 200);
    assert(pw_wine_pointer_step(&pointer, 0xff, 0xff, 50, 1000000, &move) == 1);
    assert(move.x == 130 && move.y == 150);
    int64_t x = pointer.x, y = pointer.y;
    pw_wine_pointer_resize(&pointer, 320, 400);
    assert(pointer.width == 320 && pointer.height == 400 && pointer.x == x && pointer.y == y);
    /* It then reaches the new edges. */
    assert(pw_wine_pointer_step(&pointer, 0xff, 0xff, 20000, 1000000, &move) == 1);
    assert(move.x == 319 && move.y == 399);
    /* The menu closes: the pointer is brought onto the smaller frame's last
     * pixel, and one inside stays put. */
    pw_wine_pointer_resize(&pointer, 160, 200);
    assert(pointer.x >> 16 == 159 && pointer.y >> 16 == 199);
    assert(!pw_wine_pointer_step(&pointer, 0xff, 0xff, 20000, 1000000, &move));
    pw_wine_pointer_init(&pointer, 160, 200);
    pw_wine_pointer_resize(&pointer, 100, 300);
    assert(pointer.x >> 16 == 80 && pointer.y >> 16 == 100);
    /* An empty frame is one pixel, at its origin. */
    pw_wine_pointer_resize(&pointer, 0, 0);
    assert(pointer.width == 1 && pointer.height == 1 && !pointer.x && !pointer.y);
    pw_wine_pointer_resize(NULL, 1, 1);
}

int main(void)
{
    test_frames();
    test_threads();
    test_inputs();
    test_pointer();
    test_pointer_resize();
    printf("wine display passed: frame box copy, newest frame, refusals, concurrent put/take, "
           "profile bindings to keys and mouse buttons, stick pointer, resized frames\n");
    return 0;
}
