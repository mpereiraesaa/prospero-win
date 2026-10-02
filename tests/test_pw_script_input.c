/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_script_input.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int parse(const char *text, PwScriptInput *out, size_t *bad)
{
    return pw_script_input_parse(text, strlen(text), out, bad);
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
    pw_script_input_free(&in);
    assert(!in.events && !in.count);

    /* No sync line: the replay starts with the game. */
    assert(parse("5 key 0x0d 1\n6 key 0x0d 0\n", &in, &bad) == PW_OK && !in.sync_path[0] && in.count == 2);
    pw_script_input_free(&in);
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
    };
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        assert(parse(refused[i].text, &in, &bad) == PW_ERR_MALFORMED);
        assert(bad == refused[i].line);
        assert(!in.events && !in.count);
    }
    assert(pw_script_input_parse(NULL, 0, &in, &bad) == PW_ERR_PRECONDITION);

    /* Many events grow the table. */
    {
        static char big[64 * 5000];
        size_t at = 0;
        for (int i = 0; i < 5000; i++) at += (size_t)sprintf(big + at, "%d move 1 -1\n", i);
        assert(pw_script_input_parse(big, at, &in, &bad) == PW_OK && in.count == 5000);
        assert(in.events[4999].at_ms == 4999);
        pw_script_input_free(&in);
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
    printf("pw_script_input passed: every kind, sync line, refusals with their line, growth, chunked sync search\n");
    return 0;
}
