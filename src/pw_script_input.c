/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_script_input.h"

#include <limits.h>

#include <string.h>

static const char *skip_space(const char *at, const char *end)
{
    while (at < end && (*at == ' ' || *at == '\t')) at++;
    return at;
}

/* A word: up to the next space or tab, at most capacity - 1 bytes. */
static const char *word(const char *at, const char *end, char *out, size_t capacity)
{
    size_t n = 0;

    at = skip_space(at, end);
    while (at < end && *at != ' ' && *at != '\t') {
        if (n + 1 >= capacity) return NULL;
        out[n++] = *at++;
    }
    out[n] = 0;
    return n ? at : NULL;
}

/* A decimal or 0x-hexadecimal integer, optionally negative, in [low, high]. */
static int number(const char *text, long long low, long long high, long long *value)
{
    int negative = *text == '-', base = 10, digits = 0;
    unsigned long long v = 0;

    if (negative) text++;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text += 2;
    }
    for (; *text; text++, digits++) {
        int d = *text >= '0' && *text <= '9' ? *text - '0' :
                base == 16 && *text >= 'a' && *text <= 'f' ? *text - 'a' + 10 :
                base == 16 && *text >= 'A' && *text <= 'F' ? *text - 'A' + 10 : -1;
        if (d < 0 || v > (ULLONG_MAX - (unsigned)d) / (unsigned)base) return -1;
        v = v * (unsigned)base + (unsigned)d;
    }
    if (!digits || v > (unsigned long long)LLONG_MAX) return -1;
    *value = negative ? -(long long)v : (long long)v;
    if (*value < low || *value > high) return -1;
    return 0;
}

/* A relative path inside the library root: no leading /, no .. part. */
static int safe_path(const char *path)
{
    const char *part = path;

    if (!*path || *path == '/') return 0;
    while (part) {
        if (!strncmp(part, "..", 2) && (part[2] == '/' || !part[2])) return 0;
        part = strchr(part, '/');
        if (part) part++;
    }
    return 1;
}

/* XInput's button names (XINPUT_GAMEPAD_*). */
static const struct { const char *name; uint16_t mask; } pad_names[] = {
    { "up", 0x0001 }, { "down", 0x0002 }, { "left", 0x0004 }, { "right", 0x0008 },
    { "start", 0x0010 }, { "back", 0x0020 }, { "ls", 0x0040 }, { "rs", 0x0080 },
    { "lb", 0x0100 }, { "rb", 0x0200 }, { "guide", 0x0400 },
    { "a", 0x1000 }, { "b", 0x2000 }, { "x", 0x4000 }, { "y", 0x8000 },
};

/* A button name, or a mask of the buttons XInput has. */
static int pad_buttons(const char *text, long long *mask)
{
    for (size_t i = 0; i < sizeof(pad_names) / sizeof(pad_names[0]); i++)
        if (!strcmp(text, pad_names[i].name)) {
            *mask = pad_names[i].mask;
            return 0;
        }
    return number(text, 1, 0xffff, mask) || (*mask & ~(long long)PW_SCRIPT_PAD_BUTTONS) ? -1 : 0;
}

static int parse_line(const char *at, const char *end, PwScriptInput *out, size_t capacity)
{
    char first[32], kind[16], a[32], b[32], c[32] = "";
    long long ms, x, y;
    PwScriptInputEvent event = { 0 };

    if (!(at = word(at, end, first, sizeof(first)))) return -1;
    if (!strcmp(first, "sync")) {
        size_t text_length;

        if (out->count || out->sync_path[0]) return -1;
        if (!(at = word(at, end, out->sync_path, sizeof(out->sync_path))) || !safe_path(out->sync_path))
            return -1;
        at = skip_space(at, end);
        text_length = (size_t)(end - at);
        while (text_length && (at[text_length - 1] == ' ' || at[text_length - 1] == '\t')) text_length--;
        if (!text_length || text_length >= sizeof(out->sync_text)) return -1;
        memcpy(out->sync_text, at, text_length);
        out->sync_text[text_length] = 0;
        return 0;
    }
    if (number(first, 0, UINT32_MAX, &ms) || !(at = word(at, end, kind, sizeof(kind))) ||
        !(at = word(at, end, a, sizeof(a))) || !(at = word(at, end, b, sizeof(b))))
        return -1;
    /* stick takes a third value; every other kind two. */
    if (!strcmp(kind, "stick") && !(at = word(at, end, c, sizeof(c)))) return -1;
    if (skip_space(at, end) != end) return -1;
    event.at_ms = (uint32_t)ms;
    if (!strcmp(kind, "key") && !number(a, 1, 0xfe, &x) && !number(b, 0, 1, &y)) {
        event.kind = PW_SCRIPT_INPUT_KEY;
        event.code = (uint32_t)x;
        event.down = (uint32_t)y;
    } else if (!strcmp(kind, "button") && !number(a, 0, 2, &x) && !number(b, 0, 1, &y)) {
        event.kind = PW_SCRIPT_INPUT_BUTTON;
        event.code = (uint32_t)x;
        event.down = (uint32_t)y;
    } else if (!strcmp(kind, "move") && !number(a, -32768, 32767, &x) && !number(b, -32768, 32767, &y)) {
        event.kind = PW_SCRIPT_INPUT_MOVE;
        event.dx = (int32_t)x;
        event.dy = (int32_t)y;
    } else if (!strcmp(kind, "pad") && !pad_buttons(a, &x) && !number(b, 0, 1, &y)) {
        event.kind = PW_SCRIPT_INPUT_PAD;
        event.code = (uint32_t)x;
        event.down = (uint32_t)y;
    } else if (!strcmp(kind, "stick") && (!strcmp(a, "l") || !strcmp(a, "r")) &&
               !number(b, -32768, 32767, &x) && !number(c, -32768, 32767, &y)) {
        event.kind = PW_SCRIPT_INPUT_STICK;
        event.code = a[0] == 'r';
        event.dx = (int32_t)x;
        event.dy = (int32_t)y;
    } else {
        return -1;
    }
    if (out->count && event.at_ms < out->events[out->count - 1].at_ms) return -1;
    if (out->count == capacity) return -2;
    out->events[out->count++] = event;
    if (event.kind == PW_SCRIPT_INPUT_PAD || event.kind == PW_SCRIPT_INPUT_STICK) out->pad_events++;
    return 0;
}

size_t pw_script_input_lines(const char *text, size_t length)
{
    size_t lines = length ? 1 : 0;

    for (size_t i = 0; text && i < length; i++) lines += text[i] == '\n';
    return lines;
}

int pw_script_input_parse(const char *text, size_t length, PwScriptInputEvent *storage, size_t capacity,
                          PwScriptInput *out, size_t *bad_line)
{
    const char *at = text, *end;
    size_t line = 0;

    if (!text || !out || (capacity && !storage)) return PW_ERR_PRECONDITION;
    end = text + length;
    memset(out, 0, sizeof(*out));
    out->events = storage;
    if (bad_line) *bad_line = 0;
    while (at < end) {
        const char *next = memchr(at, '\n', (size_t)(end - at)), *stop = next ? next : end;
        const char *content = skip_space(at, stop);
        int status;

        line++;
        if (stop > content && stop[-1] == '\r') stop--;
        status = content == stop || *content == '#' ? 0 : parse_line(content, stop, out, capacity);
        if (status) {
            if (bad_line) *bad_line = line;
            out->count = out->pad_events = 0;
            out->sync_path[0] = out->sync_text[0] = 0;
            return status == -2 ? PW_ERR_LIMIT : PW_ERR_MALFORMED;
        }
        at = next ? next + 1 : end;
    }
    return PW_OK;
}

int pw_script_pad_apply(PwScriptPad *pad, const PwScriptInputEvent *event, uint16_t *tick_changed)
{
    uint16_t mask, held;

    if (!pad || !event) return 0;
    if (event->kind == PW_SCRIPT_INPUT_STICK) {
        pad->stick[event->code ? 1 : 0][0] = (int16_t)event->dx;
        pad->stick[event->code ? 1 : 0][1] = (int16_t)event->dy;
        return 1;
    }
    if (event->kind != PW_SCRIPT_INPUT_PAD) return 0;
    mask = (uint16_t)event->code;
    held = event->down ? (uint16_t)(pad->buttons | mask) : (uint16_t)(pad->buttons & ~mask);
    if (tick_changed) {
        uint16_t changed = (uint16_t)(held ^ pad->buttons);
        if (changed & *tick_changed) return -1;
        *tick_changed |= changed;
    }
    pad->buttons = held;
    return 1;
}

int pw_script_input_sync_feed(PwScriptInputSync *sync, const char *chunk, size_t length)
{
    size_t need = strlen(sync->text);

    if (!need) return 1;
    for (size_t i = 0; i < length; i++) {
        if (sync->matched == need) return 1;
        /* On a mismatch, fall back to the longest start of the text that
         * also ends what matched (found directly: the text is short). */
        while (sync->matched && chunk[i] != sync->text[sync->matched]) {
            size_t shift = 1;

            while (shift < sync->matched && memcmp(sync->text + shift, sync->text, sync->matched - shift)) shift++;
            sync->matched -= shift;
        }
        if (chunk[i] == sync->text[sync->matched]) sync->matched++;
    }
    return sync->matched == need;
}
