/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../native/pw_wine_display.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static void test_frames(void)
{
    PwWineFrameBox box;
    PwPresentView view;
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
    PwPresentView view;
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
    PwWinePointer pointer = { 0, 0 };
    PwWineInput move;

    /* At rest and inside the dead zone nothing moves. */
    assert(!pw_wine_pointer_step(&pointer, 0x80, 0x80, 1200, 16000, &move));
    assert(!pw_wine_pointer_step(&pointer, 0x80 + PW_WINE_POINTER_DEADZONE, 0x80 - PW_WINE_POINTER_DEADZONE,
                                 1200, 1000000, &move));
    assert(!pointer.x && !pointer.y);
    /* Full tilt for a second is the speed's worth of motion, right or up. */
    assert(pw_wine_pointer_step(&pointer, 0xff, 0x80, 300, 1000000, &move) == 1);
    assert(move.type == PW_WINE_INPUT_MOUSE_MOVE && move.x == 300 && move.y == 0 && !move.code && !move.down);
    assert(pw_wine_pointer_step(&pointer, 0x80, 0x00, 1200, 1000000, &move) == 1);
    assert(move.x == 0 && move.y == -1200);
    /* Motion has no edge: held, it keeps coming, which is what scrolls a
     * strategy game's map with the pointer against the screen's edge. */
    for (int i = 0; i < 3; i++) {
        assert(pw_wine_pointer_step(&pointer, 0xff, 0xff, 20000, 1000000, &move) == 1);
        assert(move.x == 20000 && move.y == 20000);
    }
    /* Half tilt is slower than half speed (squared curve). */
    assert(pw_wine_pointer_step(&pointer, 0x80 + 74, 0x80, 1000, 1000000, &move) == 1);
    assert(move.x > 200 && move.x < 300 && move.y == 0);
}

static void test_pointer_fraction(void)
{
    PwWinePointer pointer = { 0, 0 };
    PwWineInput move;
    int32_t right = 0, left = 0;
    int moves = 0;

    /* Steps smaller than a pixel add up: 60 px/s for 100 frames of 16 ms is
     * 62914/65536 of a pixel a step: 95 whole pixels, sent a pixel at a time,
     * with the fraction kept. */
    for (int i = 0; i < 100; i++)
        if (pw_wine_pointer_step(&pointer, 0xff, 0x80, 60, 16000, &move)) {
            assert(move.x == 1 && move.y == 0);
            right += move.x;
            moves++;
        }
    assert(right == 95 && moves == 95 && pointer.x > 0 && pointer.x < 65536);
    /* Left is the mirror image: the fraction rounds towards zero both ways. */
    pointer = (PwWinePointer){ 0, 0 };
    for (int i = 0; i < 100; i++)
        if (pw_wine_pointer_step(&pointer, 0x01, 0x80, 60, 16000, &move)) left += move.x;
    assert(left == -95 && pointer.x < 0 && pointer.x > -65536);
    assert(!pw_wine_pointer_step(NULL, 0xff, 0x80, 1, 1, &move) && !pw_wine_pointer_step(&pointer, 0, 0, 1, 1, NULL));
}

static void test_pad(void)
{
    PwPadPs5 pad;
    PwWinePad xinput;

    memset(&pad, 0, sizeof(pad));
    pad.left_stick = pad.right_stick = (PwPadPs5Stick){ 0x80, 0x80 };
    /* Disconnected: nothing, and a neutral state. */
    memset(&xinput, 0x5a, sizeof(xinput));
    assert(pw_wine_game_pad(&pad, &xinput) == 0);
    assert(!xinput.connected && !xinput.buttons && !xinput.thumb_lx && !xinput.left_trigger);
    assert(pw_wine_game_pad(NULL, &xinput) == 0 && pw_wine_game_pad(&pad, NULL) == 0);

    /* Centred and released. */
    pad.core.connected = 1;
    assert(pw_wine_game_pad(&pad, &xinput) == 1 && xinput.connected == 1 && xinput.packet == 0);
    assert(!xinput.buttons && !xinput.left_trigger && !xinput.right_trigger);
    assert(!xinput.thumb_lx && !xinput.thumb_ly && !xinput.thumb_rx && !xinput.thumb_ry);

    /* Each DualSense button, alone, as its XInput bit. */
    static const struct { uint32_t dualsense; uint16_t xinput; } buttons[] = {
        { 0x4000u, 0x1000u /* cross: A */ }, { 0x2000u, 0x2000u /* circle: B */ },
        { 0x8000u, 0x4000u /* square: X */ }, { 0x1000u, 0x8000u /* triangle: Y */ },
        { 0x400u, 0x0100u /* l1 */ }, { 0x800u, 0x0200u /* r1 */ },
        { 0x2u, 0x0040u /* l3 */ }, { 0x4u, 0x0080u /* r3 */ },
        { 0x10u, 0x0001u /* up */ }, { 0x40u, 0x0002u /* down */ },
        { 0x80u, 0x0004u /* left */ }, { 0x20u, 0x0008u /* right */ },
        { 0x8u, 0x0010u /* options: start */ }, { 0x1u, 0x0020u /* create: back */ },
        { 0x100000u, 0x0400u /* touchpad: guide */ },
    };
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
        pad.core.previous_buttons = buttons[i].dualsense;
        assert(pw_wine_game_pad(&pad, &xinput) == 1 && xinput.buttons == buttons[i].xinput);
    }
    /* L2/R2 are analog triggers, not buttons; unknown bits are dropped. */
    pad.core.previous_buttons = 0x100u | 0x200u | 0x80000000u;
    pad.l2 = 1;
    pad.r2 = 255;
    assert(pw_wine_game_pad(&pad, &xinput) == 1 && !xinput.buttons);
    assert(xinput.left_trigger == 1 && xinput.right_trigger == 255);
    pad.core.previous_buttons = 0x4000u | 0x8u | 0x10u;
    assert(pw_wine_game_pad(&pad, &xinput) == 1 && xinput.buttons == (0x1000u | 0x0010u | 0x0001u));

    /* Sticks: full reach both ways, centre 0, y up. */
    pad.left_stick = (PwPadPs5Stick){ 0x00, 0x00 };     /* left, up */
    pad.right_stick = (PwPadPs5Stick){ 0xff, 0xff };    /* right, down */
    assert(pw_wine_game_pad(&pad, &xinput) == 1);
    assert(xinput.thumb_lx == -32768 && xinput.thumb_ly == 32767);
    assert(xinput.thumb_rx == 32767 && xinput.thumb_ry == -32768);
    pad.left_stick = (PwPadPs5Stick){ 0x81, 0x7f };     /* a step right, a step up */
    pad.right_stick = (PwPadPs5Stick){ 0x7f, 0x81 };
    assert(pw_wine_game_pad(&pad, &xinput) == 1);
    assert(xinput.thumb_lx == 32767 / 127 && xinput.thumb_ly == 32767 / 128);
    assert(xinput.thumb_rx == -32768 / 128 && xinput.thumb_ry == -32768 / 127);
    pad.left_stick = (PwPadPs5Stick){ 0xc0, 0x40 };     /* halfway right and up */
    assert(pw_wine_game_pad(&pad, &xinput) == 1);
    assert(xinput.thumb_lx == 64 * 32767 / 127 && xinput.thumb_ly == 64 * 32767 / 128);
}

/* A script build's controller added to the DualSense's. */
static void test_script_pad(void)
{
    PwPadPs5 pad;
    PwWinePad xinput;
    PwScriptPad script = { 0 };

    memset(&pad, 0, sizeof(pad));
    pad.core.connected = 1;
    pad.core.previous_buttons = 0x2000u;                /* circle: B */
    pad.left_stick = (PwPadPs5Stick){ 0xff, 0x80 };     /* full right */
    pad.right_stick = (PwPadPs5Stick){ 0x80, 0x00 };    /* full up */
    pad.r2 = 200;

    /* Nothing scripted: the real controller as it is. */
    assert(pw_wine_game_pad(&pad, &xinput) == 1);
    assert(pw_wine_script_pad(&xinput, 1, &script, 1) == 1);
    assert(xinput.buttons == 0x2000u && xinput.thumb_lx == 32767 && xinput.thumb_ly == 0 &&
           xinput.thumb_ry == 32767 && xinput.right_trigger == 200);

    /* Buttons join; a stick held off centre replaces the real one, and
     * only that stick. */
    script.buttons = 0x1000u | 0x2000u;                 /* A, and B both ways */
    script.stick[0][0] = -16000;
    script.stick[0][1] = 0;
    assert(pw_wine_game_pad(&pad, &xinput) == 1);
    assert(pw_wine_script_pad(&xinput, 1, &script, 1) == 1);
    assert(xinput.connected == 1 && xinput.buttons == 0x3000u);
    assert(xinput.thumb_lx == -16000 && xinput.thumb_ly == 0);
    assert(xinput.thumb_rx == 0 && xinput.thumb_ry == 32767 && xinput.right_trigger == 200);
    script.stick[0][0] = 0;
    script.stick[1][1] = -1;                            /* any off-centre value counts */
    assert(pw_wine_game_pad(&pad, &xinput) == 1);
    assert(pw_wine_script_pad(&xinput, 1, &script, 1) == 1);
    assert(xinput.thumb_lx == 32767 && xinput.thumb_rx == 0 && xinput.thumb_ry == -1);

    /* No DualSense: the script's controller alone, still connected while it
     * holds nothing. */
    pad.core.connected = 0;
    memset(&script, 0, sizeof(script));
    assert(pw_wine_game_pad(&pad, &xinput) == 0);
    assert(pw_wine_script_pad(&xinput, 0, &script, 1) == 1);
    assert(xinput.connected == 1 && !xinput.buttons && !xinput.thumb_lx && !xinput.right_trigger);
    script.buttons = 0x0010u;
    memset(&xinput, 0x5a, sizeof(xinput));              /* stale: cleared when not connected */
    assert(pw_wine_script_pad(&xinput, 0, &script, 1) == 1);
    assert(xinput.buttons == 0x0010u && !xinput.thumb_lx && !xinput.left_trigger && !xinput.packet);

    /* A macro without pad events leaves controller 0 to the DualSense. */
    assert(pw_wine_script_pad(&xinput, 0, &script, 0) == 0 && !xinput.connected && !xinput.buttons);
    pad.core.connected = 1;
    assert(pw_wine_game_pad(&pad, &xinput) == 1);
    assert(pw_wine_script_pad(&xinput, 1, &script, 0) == 1 && xinput.buttons == 0x2000u);
    assert(pw_wine_script_pad(&xinput, 1, NULL, 1) == 1 && xinput.buttons == 0x2000u);
    assert(pw_wine_script_pad(NULL, 1, &script, 1) == 0);

    /* From a parsed macro, through the scripted controller, to XInput. */
    {
        static PwScriptInputEvent storage[4];
        static const char macro[] = "0 pad a 1\n0 stick l 0 32767\n";
        PwScriptInput in;
        size_t bad;
        uint16_t changed = 0;

        memset(&script, 0, sizeof(script));
        assert(pw_script_input_parse(macro, sizeof(macro) - 1, storage, 4, &in, &bad) == PW_OK);
        for (size_t i = 0; i < in.count; i++) assert(pw_script_pad_apply(&script, &in.events[i], &changed) == 1);
        pad.core.connected = 0;
        assert(pw_wine_game_pad(&pad, &xinput) == 0);
        assert(pw_wine_script_pad(&xinput, 0, &script, in.pad_events != 0) == 1);
        assert(xinput.buttons == 0x1000u && xinput.thumb_lx == 0 && xinput.thumb_ly == 32767);
    }
}

int main(void)
{
    test_frames();
    test_threads();
    test_inputs();
    test_pointer();
    test_pointer_fraction();
    test_pad();
    test_script_pad();
    printf("wine display passed: frame box copy, newest frame, refusals, concurrent put/take, "
           "profile bindings to keys and mouse buttons, stick pointer, resized frames, "
           "DualSense as XInput, scripted controller added to it\n");
    return 0;
}
