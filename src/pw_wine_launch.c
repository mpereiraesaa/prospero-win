/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_wine_launch.h"
#include <string.h>

static const char *value_of(const char *word, const char *key)
{
    size_t length = strlen(key);

    return !strncmp(word, key, length) && word[length] == '=' ? word + length + 1 : NULL;
}

/* An absolute Windows path: a drive letter, ':' and '\'. */
static int valid_path(const char *path)
{
    size_t length = strlen(path);

    return length >= 3 && length < PW_WINE_LAUNCH_PATH_MAX &&
           ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
           path[1] == ':' && path[2] == '\\';
}

static int parse_count(const char *text, uint32_t *out)
{
    uint64_t value = 0;

    if (!*text) return -1;
    for (; *text; text++) {
        if (*text < '0' || *text > '9') return -1;
        if ((value = value * 10u + (uint64_t)(*text - '0')) > UINT32_MAX) return -1;
    }
    *out = (uint32_t)value;
    return 0;
}

int pw_wine_launch_parse(int argc, char *const *argv, const PwWineApp *apps, size_t count,
                         PwWineLaunch *out)
{
    const char *profile = NULL, *path = NULL;
    int sync = 0;

    if (!out || (!apps && count)) return -1;
    memset(out, 0, sizeof(*out));
    out->mode = PW_WINE_LAUNCH_LAUNCHER;
    for (int i = 0; argv && i < argc; i++) {
        const char *word = argv[i], *value;
        if (!word) continue;
        if ((value = value_of(word, "profile"))) profile = value;
        else if ((value = value_of(word, "path"))) path = value;
        else if ((value = value_of(word, "sync"))) sync = !strcmp(value, "1");
        else if ((value = value_of(word, "cycle")) && parse_count(value, &out->cycle)) out->cycle = 0;
    }
    if (profile) {
        for (size_t i = 0; i < count && !out->app; i++)
            if (apps[i].id && !strcmp(apps[i].id, profile)) out->app = &apps[i];
        if (!out->app) { out->refused = 1; return 0; }
        if (!path) path = out->app->executable;
    }
    if (!path) {
        if (sync) out->mode = PW_WINE_LAUNCH_SYNC;
        return 0;
    }
    if (!valid_path(path)) { out->app = NULL; out->refused = 1; return 0; }
    memcpy(out->executable, path, strlen(path) + 1);
    out->mode = PW_WINE_LAUNCH_GAME;
    return 0;
}

/* Append "key=value" to storage at *used; 0, or -1 when it does not fit. */
static int add_word(char *storage, size_t size, size_t *used, const char *key, const char *value)
{
    size_t key_length = strlen(key), value_length = strlen(value);

    if (*used + key_length + 1 + value_length + 1 > size) return -1;
    memcpy(storage + *used, key, key_length);
    storage[*used + key_length] = '=';
    memcpy(storage + *used + key_length + 1, value, value_length + 1);
    *used += key_length + 1 + value_length + 1;
    return 0;
}

/* app's words, or key=1 without one, then the cycle. */
static size_t launch_words(const PwWineApp *app, const char *key, uint32_t cycle, char *storage,
                           size_t size, char **argv, size_t max)
{
    char digits[11], number[11];
    size_t used = 0, words = 0, n = 0;

    if (!storage || !argv || max < PW_WINE_LAUNCH_ARGS) return 0;
    do digits[n++] = (char)('0' + cycle % 10u); while ((cycle /= 10u) && n < sizeof(digits));
    for (size_t i = 0; i < n; i++) number[i] = digits[n - 1 - i];
    number[n] = 0;
    if (app) {
        if (!app->id || !app->executable) return 0;
        argv[words++] = storage + used;
        if (add_word(storage, size, &used, "profile", app->id)) return 0;
        argv[words++] = storage + used;
        if (add_word(storage, size, &used, "path", app->executable)) return 0;
    } else {
        argv[words++] = storage + used;
        if (add_word(storage, size, &used, key, "1")) return 0;
    }
    argv[words++] = storage + used;
    if (add_word(storage, size, &used, "cycle", number)) return 0;
    argv[words] = NULL;
    return words;
}

size_t pw_wine_launch_argv(const PwWineApp *app, uint32_t cycle, char *storage, size_t size,
                           char **argv, size_t max)
{
    return launch_words(app, "launcher", cycle, storage, size, argv, max);
}

size_t pw_wine_launch_sync_argv(uint32_t cycle, char *storage, size_t size, char **argv, size_t max)
{
    return launch_words(NULL, "sync", cycle, storage, size, argv, max);
}

int pw_wine_launch_needs_data(int argc, char *const *argv)
{
    for (int i = 0; argv && i < argc; i++) {
        const char *word = argv[i], *value;
        if (!word) continue;
        if (value_of(word, "profile") || value_of(word, "path")) return 1;
        if ((value = value_of(word, "sync")) && !strcmp(value, "1")) return 1;
    }
    return 0;
}
