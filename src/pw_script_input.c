/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_script_input.h"

#include <stdlib.h>
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

static int number(const char *text, long long low, long long high, long long *value)
{
    char *stop;
    long long v = strtoll(text, &stop, 0);

    if (stop == text || *stop || v < low || v > high) return -1;
    *value = v;
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

static int parse_line(const char *at, const char *end, PwScriptInput *out, size_t *capacity)
{
    char first[32], kind[16], a[32], b[32];
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
        !(at = word(at, end, a, sizeof(a))) || !(at = word(at, end, b, sizeof(b))) ||
        skip_space(at, end) != end)
        return -1;
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
    } else {
        return -1;
    }
    if (out->count && event.at_ms < out->events[out->count - 1].at_ms) return -1;
    if (out->count == *capacity) {
        size_t grown = *capacity ? *capacity * 2 : 1024;
        PwScriptInputEvent *events;

        if (grown > PW_SCRIPT_INPUT_MAX_EVENTS) grown = PW_SCRIPT_INPUT_MAX_EVENTS;
        if (out->count == grown || !(events = realloc(out->events, grown * sizeof(*events)))) return -2;
        out->events = events;
        *capacity = grown;
    }
    out->events[out->count++] = event;
    return 0;
}

int pw_script_input_parse(const char *text, size_t length, PwScriptInput *out, size_t *bad_line)
{
    const char *at = text, *end = text + length;
    size_t capacity = 0, line = 0;

    if (!text || !out) return PW_ERR_PRECONDITION;
    memset(out, 0, sizeof(*out));
    if (bad_line) *bad_line = 0;
    while (at < end) {
        const char *next = memchr(at, '\n', (size_t)(end - at)), *stop = next ? next : end;
        const char *content = skip_space(at, stop);
        int status;

        line++;
        if (stop > content && stop[-1] == '\r') stop--;
        status = content == stop || *content == '#' ? 0 : parse_line(content, stop, out, &capacity);
        if (status) {
            if (bad_line) *bad_line = line;
            pw_script_input_free(out);
            return status == -2 ? PW_ERR_LIMIT : PW_ERR_MALFORMED;
        }
        at = next ? next + 1 : end;
    }
    return PW_OK;
}

void pw_script_input_free(PwScriptInput *input)
{
    if (!input) return;
    free(input->events);
    memset(input, 0, sizeof(*input));
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
