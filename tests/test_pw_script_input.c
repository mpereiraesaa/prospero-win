/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_script_input.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static PwScriptInputEvent storage[8192];

static int parse(const char *text, PwScriptInput *out, size_t *bad)
{
    return pw_script_input_parse(text, strlen(text), storage, 64, out, bad);
}

int main(void)
{
    PwScriptInput in;
    size_t bad;

    /* Every kind, the sync line, comments, blank lines and CRLF. */
    assert(parse("sync prefixes/half-life-2/drive_c/Games/HalfLife2/hl2/console.log Redownloading all lightmaps\r\n"
                 "# recorded on the PC\n\n"
                 "0 key 0x57 1\n"
                 "16 move -3 12\n"
                 "16 button 0 1\r\n"
                 "40 button 0 0\n"
                 "1200 key 87 0", &in, &bad) == PW_OK);
    assert(!strcmp(in.sync_path, "prefixes/half-life-2/drive_c/Games/HalfLife2/hl2/console.log"));
    assert(!strcmp(in.sync_text, "Redownloading all lightmaps"));
    assert(in.count == 5);
    assert(in.events[0].kind == PW_SCRIPT_INPUT_KEY && in.events[0].code == 0x57 && in.events[0].down == 1);
    assert(in.events[1].kind == PW_SCRIPT_INPUT_MOVE && in.events[1].dx == -3 && in.events[1].dy == 12 &&
           in.events[1].at_ms == 16);
    assert(in.events[2].kind == PW_SCRIPT_INPUT_BUTTON && in.events[2].code == 0 && in.events[2].down == 1);
    assert(in.events[4].at_ms == 1200 && in.events[4].code == 87 && in.events[4].down == 0);
    assert(pw_script_input_lines("a\nb\nc", 5) == 3 && pw_script_input_lines("", 0) == 0);

    /* No sync line: the replay starts with the game. */
    assert(parse("5 key 0x0d 1\n6 key 0x0d 0\n", &in, &bad) == PW_OK && !in.sync_path[0] && in.count == 2);
    assert(parse("", &in, &bad) == PW_OK && in.count == 0);

    /* Refused, with the line: time going back, unknown kinds, out-of-range
     * values, trailing words, a sync after events or escaping the root. */
    static const struct { const char *text; size_t line; } refused[] = {
        { "10 key 0x41 1\n5 key 0x41 0\n", 2 },
        { "1 jump 1 1\n", 1 },
        { "1 key 0x41 2\n", 1 },
        { "1 key 0 1\n", 1 },
        { "1 key 0x100 1\n", 1 },
        { "1 button 3 1\n", 1 },
        { "1 move 40000 0\n", 1 },
        { "1 move 1\n", 1 },
        { "1 move 1 2 3\n", 1 },
        { "-1 key 0x41 1\n", 1 },
        { "x key 0x41 1\n", 1 },
        { "1 key 0x41 1\nsync a.log text\n", 2 },
        { "sync /data/x.log text\n", 1 },
        { "sync a/../../x.log text\n", 1 },
        { "sync ../x.log text\n", 1 },
        { "sync a.log\n", 1 },
        { "sync a.log t\nsync b.log t\n", 2 },
        { "1 pad A 1\n", 1 },          /* names are lower case */
        { "1 pad select 1\n", 1 },
        { "1 pad 0 1\n", 1 },          /* no button */
        { "1 pad 0x0800 1\n", 1 },     /* not an XInput button */
        { "1 pad 0x10000 1\n", 1 },
        { "1 pad -1 1\n", 1 },
        { "1 pad a 2\n", 1 },
        { "1 pad a\n", 1 },
        { "1 pad a 1 1\n", 1 },
        { "1 pad a 1\n0 pad a 0\n", 2 },
        { "1 stick l 0\n", 1 },
        { "1 stick l 0 0 0\n", 1 },
        { "1 stick c 0 0\n", 1 },
        { "1 stick L 0 0\n", 1 },
        { "1 stick l 32768 0\n", 1 },
        { "1 stick r 0 -32769\n", 1 },
        { "1 move 1 2\n2 pad a 1\n3 pad b 1\n4 stick x 1 1\n", 4 },
    };
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        assert(parse(refused[i].text, &in, &bad) == PW_ERR_MALFORMED);
        assert(bad == refused[i].line);
        assert(!in.count && !in.pad_events && !in.sync_path[0]);
    }
    assert(pw_script_input_parse(NULL, 0, storage, 1, &in, &bad) == PW_ERR_PRECONDITION);
    assert(pw_script_input_parse("1 key 0x41 1", 12, NULL, 1, &in, &bad) == PW_ERR_PRECONDITION);
    /* Past the caller's storage: refused with the line, not overrun. */
    assert(pw_script_input_parse("1 key 0x41 1\n2 key 0x41 0\n3 move 1 1\n", 39, storage, 2, &in, &bad) ==
           PW_ERR_LIMIT && bad == 3 && in.count == 0);
    /* Numbers: decimal, hexadecimal, negative; junk and overflow refused. */
    assert(parse("1 move -32768 0x7fff\n", &in, &bad) == PW_OK && in.events[0].dx == -32768 &&
           in.events[0].dy == 32767);
    assert(parse("99999999999999999999999 key 0x41 1\n", &in, &bad) == PW_ERR_MALFORMED);
    assert(parse("1 key 0x 1\n", &in, &bad) == PW_ERR_MALFORMED);
    assert(parse("1 key 0x4g 1\n", &in, &bad) == PW_ERR_MALFORMED);

    /* Pad buttons by name and mask, held until released; sticks. */
    assert(parse("0 pad a 1\n"
                 "150 pad a 0\n"
                 "150 pad 0x1010 1\n"
                 "200 stick l -32768 32767\n"
                 "200 stick r 0 -1\n"
                 "300 pad start 0\n"
                 "300 key 0x0d 1\n", &in, &bad) == PW_OK);
    assert(in.count == 7 && in.pad_events == 6);
    assert(in.events[0].kind == PW_SCRIPT_INPUT_PAD && in.events[0].code == 0x1000 && in.events[0].down == 1);
    assert(in.events[1].kind == PW_SCRIPT_INPUT_PAD && in.events[1].code == 0x1000 && in.events[1].down == 0 &&
           in.events[1].at_ms == 150);
    assert(in.events[2].code == 0x1010 && in.events[2].down == 1);
    assert(in.events[3].kind == PW_SCRIPT_INPUT_STICK && in.events[3].code == 0 && in.events[3].dx == -32768 &&
           in.events[3].dy == 32767);
    assert(in.events[4].kind == PW_SCRIPT_INPUT_STICK && in.events[4].code == 1 && in.events[4].dx == 0 &&
           in.events[4].dy == -1);
    assert(in.events[5].code == 0x0010 && in.events[5].down == 0);
    {
        static const struct { const char *name; uint32_t mask; } names[] = {
            { "a", 0x1000 }, { "b", 0x2000 }, { "x", 0x4000 }, { "y", 0x8000 }, { "start", 0x0010 },
            { "back", 0x0020 }, { "lb", 0x0100 }, { "rb", 0x0200 }, { "ls", 0x0040 }, { "rs", 0x0080 },
            { "up", 0x0001 }, { "down", 0x0002 }, { "left", 0x0004 }, { "right", 0x0008 }, { "guide", 0x0400 },
            { "0xf7ff", 0xf7ff }, { "4096", 0x1000 },
        };
        char line[64];
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            snprintf(line, sizeof(line), "5 pad %s 1\n", names[i].name);
            assert(parse(line, &in, &bad) == PW_OK && in.count == 1 && in.events[0].code == names[i].mask);
        }
    }
    /* Keys and mouse only: no controller. */
    assert(parse("1 key 0x41 1\n", &in, &bad) == PW_OK && in.pad_events == 0);

    /* Many events. */
    {
        static char big[64 * 5000];
        size_t at = 0;
        for (int i = 0; i < 5000; i++) at += (size_t)sprintf(big + at, "%d move 1 -1\n", i);
        assert(pw_script_input_lines(big, at) == 5001);
        assert(pw_script_input_parse(big, at, storage, 8192, &in, &bad) == PW_OK && in.count == 5000);
        assert(in.events[4999].at_ms == 4999 && in.events == storage);
    }

    /* Sync: found in one chunk, across chunks, after a false start, never. */
    {
        PwScriptInputSync sync = { "lightmaps", 0 };
        assert(!pw_script_input_sync_feed(&sync, "Redownloading all light", 23));
        assert(pw_script_input_sync_feed(&sync, "maps\n", 5));
        sync = (PwScriptInputSync){ "aab", 0 };
        assert(pw_script_input_sync_feed(&sync, "aaab", 4));
        sync = (PwScriptInputSync){ "abab", 0 };
        assert(!pw_script_input_sync_feed(&sync, "abaab", 5));
        assert(pw_script_input_sync_feed(&sync, "ab", 2));
        sync = (PwScriptInputSync){ "loaded", 0 };
        assert(!pw_script_input_sync_feed(&sync, "load", 4) && !pw_script_input_sync_feed(&sync, "ing...", 6));
    }
    /* The scripted controller: presses hold until released, sticks until
     * moved again; a second change of a button in one frame waits. */
    {
        PwScriptPad pad = { 0 };
        uint16_t changed = 0;
        PwScriptInputEvent press_a = { 0, PW_SCRIPT_INPUT_PAD, 0x1000, 0, 0, 1 };
        PwScriptInputEvent release_a = { 0, PW_SCRIPT_INPUT_PAD, 0x1000, 0, 0, 0 };
        PwScriptInputEvent press_start_a = { 0, PW_SCRIPT_INPUT_PAD, 0x1010, 0, 0, 1 };
        PwScriptInputEvent press_b = { 0, PW_SCRIPT_INPUT_PAD, 0x2000, 0, 0, 1 };
        PwScriptInputEvent stick_r = { 0, PW_SCRIPT_INPUT_STICK, 1, 1200, -900, 0 };
        PwScriptInputEvent stick_l = { 0, PW_SCRIPT_INPUT_STICK, 0, -5, 7, 0 };
        PwScriptInputEvent stick_l_off = { 0, PW_SCRIPT_INPUT_STICK, 0, 0, 0, 0 };
        PwScriptInputEvent key = { 0, PW_SCRIPT_INPUT_KEY, 0x0d, 0, 0, 1 };

        assert(pw_script_pad_apply(&pad, &key, &changed) == 0 && !pad.buttons && !changed);
        assert(pw_script_pad_apply(&pad, &press_a, &changed) == 1 && pad.buttons == 0x1000 && changed == 0x1000);
        /* Released in the frame of its press: refused, still down. */
        assert(pw_script_pad_apply(&pad, &release_a, &changed) == -1 && pad.buttons == 0x1000);
        /* Another button the same frame is fine. */
        assert(pw_script_pad_apply(&pad, &press_b, &changed) == 1 && pad.buttons == 0x3000 && changed == 0x3000);
        /* Pressing a held button again changes nothing, so it never waits. */
        assert(pw_script_pad_apply(&pad, &press_a, &changed) == 1 && pad.buttons == 0x3000);
        changed = 0; /* the next frame */
        assert(pw_script_pad_apply(&pad, &release_a, &changed) == 1 && pad.buttons == 0x2000 && changed == 0x1000);
        /* A mask presses several; one already held is not a change. */
        assert(pw_script_pad_apply(&pad, &press_start_a, &changed) == -1 && pad.buttons == 0x2000);
        changed = 0;
        assert(pw_script_pad_apply(&pad, &press_start_a, &changed) == 1 && pad.buttons == 0x3010 && changed == 0x1010);
        /* Without a frame mask (NULL), every event applies at once. */
        assert(pw_script_pad_apply(&pad, &release_a, NULL) == 1 && pad.buttons == 0x2010);
        assert(pw_script_pad_apply(&pad, &stick_r, &changed) == 1 && pad.stick[1][0] == 1200 &&
               pad.stick[1][1] == -900 && pad.stick[0][0] == 0);
        assert(pw_script_pad_apply(&pad, &stick_l, &changed) == 1 && pad.stick[0][0] == -5 && pad.stick[0][1] == 7);
        assert(pw_script_pad_apply(&pad, &stick_l_off, &changed) == 1 && !pad.stick[0][0] && !pad.stick[0][1] &&
               pad.stick[1][0] == 1200);
        assert(pw_script_pad_apply(NULL, &press_a, &changed) == 0 && pw_script_pad_apply(&pad, NULL, &changed) == 0);

        /* A parsed press-and-hold, replayed a frame (16 ms) at a time as the
         * script build does: A is down from 0 ms through the frame before
         * 150 ms; a zero-length press still lasts one frame. */
        assert(parse("0 pad a 1\n150 pad a 0\n300 pad b 1\n300 pad b 0\n", &in, &bad) == PW_OK);
        memset(&pad, 0, sizeof(pad));
        size_t next = 0;
        int a_frames = 0, b_frames = 0;
        for (uint32_t now = 0; now < 400; now += 16) {
            changed = 0;
            while (next < in.count && in.events[next].at_ms <= now) {
                if (pw_script_pad_apply(&pad, &in.events[next], &changed) < 0) break;
                next++;
            }
            a_frames += (pad.buttons & 0x1000) != 0;
            b_frames += (pad.buttons & 0x2000) != 0;
        }
        assert(next == in.count && !pad.buttons && a_frames == 10 && b_frames == 1);
    }
    printf("pw_script_input passed: every kind, sync line, refusals with their line, the caller's capacity, numbers, chunked sync search, pad names and masks, held pad buttons and sticks\n");
    return 0;
}
