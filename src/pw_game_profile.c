/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_game_profile.h"
#include <string.h>

enum { SET_MODE = 1u << 0, SET_MOUSE = 1u << 1, SET_SPEED = 1u << 2, SET_PRESET = 1u << 3,
       SET_PLAYER2 = 1u << 4 };

/* DualSense buttons in PW_GAME_BUTTON order, with their scePadRead bits. */
static const struct { const char *name; uint32_t mask; } buttons[PW_GAME_BUTTON_COUNT] = {
    { "cross", 0x4000u }, { "circle", 0x2000u }, { "square", 0x8000u }, { "triangle", 0x1000u },
    { "l1", 0x400u }, { "r1", 0x800u }, { "l2", 0x100u }, { "r2", 0x200u },
    { "l3", 0x2u }, { "r3", 0x4u }, { "up", 0x10u }, { "down", 0x40u },
    { "left", 0x80u }, { "right", 0x20u }, { "options", 0x8u }, { "create", 0x1u },
    { "touchpad", 0x100000u },
};

/* Windows virtual keys by name. */
static const struct { const char *name; uint16_t vk; } keys[] = {
    { "space", 0x20 }, { "enter", 0x0d }, { "escape", 0x1b }, { "tab", 0x09 },
    { "backspace", 0x08 }, { "shift", 0x10 }, { "ctrl", 0x11 }, { "alt", 0x12 },
    { "pause", 0x13 }, { "pageup", 0x21 }, { "pagedown", 0x22 }, { "end", 0x23 },
    { "home", 0x24 }, { "left", 0x25 }, { "up", 0x26 }, { "right", 0x27 }, { "down", 0x28 },
    { "insert", 0x2d }, { "delete", 0x2e }, { "semicolon", 0xba }, { "equals", 0xbb },
    { "comma", 0xbc }, { "minus", 0xbd }, { "period", 0xbe }, { "slash", 0xbf },
    { "backquote", 0xc0 }, { "lbracket", 0xdb }, { "backslash", 0xdc }, { "rbracket", 0xdd },
    { "quote", 0xde },
};

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

/* Case-insensitive match of [text, text+length) against a NUL-terminated word. */
static int is(const uint8_t *text, size_t length, const char *word)
{
    size_t n = strlen(word);

    if (n != length) return 0;
    for (size_t i = 0; i < n; i++) if (lower((char)text[i]) != word[i]) return 0;
    return 1;
}

static void trim(const uint8_t **begin, const uint8_t **end)
{
    while (*begin < *end && (**begin == ' ' || **begin == '\t')) ++*begin;
    while (*end > *begin && ((*end)[-1] == ' ' || (*end)[-1] == '\t')) --*end;
}

static int number(const uint8_t *text, size_t length, uint32_t *out)
{
    uint64_t value = 0;

    if (!length || length > 9) return PW_ERR_MALFORMED;
    for (size_t i = 0; i < length; i++) {
        if (text[i] < '0' || text[i] > '9') return PW_ERR_MALFORMED;
        value = value * 10u + (uint64_t)(text[i] - '0');
    }
    *out = (uint32_t)value;
    return PW_OK;
}

static int hex_digit(uint8_t c)
{
    c = (uint8_t)lower((char)c);
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* A key name, a letter, a digit, f1-f24, vk:0xNN, mouse_* or none. */
static int binding(const uint8_t *v, size_t n, PwGameBinding *out)
{
    if (is(v, n, "none")) { out->kind = PW_GAME_BIND_NONE; out->code = 0; return PW_OK; }
    if (is(v, n, "mouse_left") || is(v, n, "mouse_right") || is(v, n, "mouse_middle")) {
        out->kind = PW_GAME_BIND_MOUSE;
        out->code = is(v, n, "mouse_left") ? 0 : is(v, n, "mouse_right") ? 1 : 2;
        return PW_OK;
    }
    out->kind = PW_GAME_BIND_KEY;
    if (n == 1) {
        char c = lower((char)v[0]);
        if (c >= 'a' && c <= 'z') { out->code = (uint16_t)(c - 'a' + 'A'); return PW_OK; }
        if (c >= '0' && c <= '9') { out->code = (uint16_t)c; return PW_OK; }
        return PW_ERR_UNSUPPORTED;
    }
    if ((n == 2 || n == 3) && lower((char)v[0]) == 'f') {
        uint32_t f;
        if (number(v + 1, n - 1, &f) == PW_OK && f >= 1 && f <= 24) {
            out->code = (uint16_t)(0x70 + f - 1);
            return PW_OK;
        }
        return PW_ERR_UNSUPPORTED;
    }
    if (n == 7 && is(v, 5, "vk:0x")) {
        int high = hex_digit(v[5]), low = hex_digit(v[6]);
        if (high < 0 || low < 0 || !(high | low)) return PW_ERR_UNSUPPORTED;
        out->code = (uint16_t)(high * 16 + low);
        return PW_OK;
    }
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (is(v, n, keys[i].name)) { out->code = keys[i].vk; return PW_OK; }
    return PW_ERR_UNSUPPORTED;
}

/* A preset's name: the file name under input/ without .input. */
static int preset_name(char *out, size_t capacity, const uint8_t *v, size_t n)
{
    if (!n || n >= capacity) return PW_ERR_MALFORMED;
    for (size_t i = 0; i < n; i++) {
        char c = (char)v[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return PW_ERR_MALFORMED;
        out[i] = c;
    }
    out[n] = 0;
    return PW_OK;
}

static int input_field(PwGameInput *input, const uint8_t *key, size_t key_length,
                       const uint8_t *v, size_t n, int allow_preset)
{
    if (is(key, key_length, "preset")) {
        if (!allow_preset || (input->set & SET_PRESET)) return PW_ERR_MALFORMED;
        if (preset_name(input->preset, sizeof(input->preset), v, n) != PW_OK) return PW_ERR_MALFORMED;
        input->set |= SET_PRESET;
        return PW_OK;
    }
    if (is(key, key_length, "player2")) {
        if (!allow_preset || (input->set & SET_PLAYER2)) return PW_ERR_MALFORMED;
        if (preset_name(input->player2, sizeof(input->player2), v, n) != PW_OK) return PW_ERR_MALFORMED;
        input->set |= SET_PLAYER2;
        return PW_OK;
    }
    if (is(key, key_length, "mode")) {
        if (input->set & SET_MODE) return PW_ERR_MALFORMED;
        if (is(v, n, "keyboard")) input->mode = PW_GAME_INPUT_KEYBOARD;
        else if (is(v, n, "xinput")) input->mode = PW_GAME_INPUT_XINPUT;
        else return PW_ERR_UNSUPPORTED;
        input->set |= SET_MODE;
        return PW_OK;
    }
    if (is(key, key_length, "mouse")) {
        if (input->set & SET_MOUSE) return PW_ERR_MALFORMED;
        if (is(v, n, "none")) input->mouse = PW_GAME_STICK_NONE;
        else if (is(v, n, "left_stick")) input->mouse = PW_GAME_STICK_LEFT;
        else if (is(v, n, "right_stick")) input->mouse = PW_GAME_STICK_RIGHT;
        else return PW_ERR_UNSUPPORTED;
        input->set |= SET_MOUSE;
        return PW_OK;
    }
    if (is(key, key_length, "mouse_speed")) {
        uint32_t speed;
        if ((input->set & SET_SPEED) || number(v, n, &speed) != PW_OK || !speed ||
            speed > PW_GAME_MOUSE_SPEED_MAX)
            return PW_ERR_MALFORMED;
        input->mouse_speed = speed;
        input->set |= SET_SPEED;
        return PW_OK;
    }
    for (size_t i = 0; i < PW_GAME_BUTTON_COUNT; i++) {
        if (!is(key, key_length, buttons[i].name)) continue;
        PwGameBinding parsed = { buttons[i].mask, 0, 0 };
        if (input->bindings[i].kind != PW_GAME_BIND_UNSET) return PW_ERR_MALFORMED;
        int status = binding(v, n, &parsed);
        if (status == PW_OK) input->bindings[i] = parsed;
        return status;
    }
    return PW_ERR_UNSUPPORTED;
}

static int display_field(PwGameDisplay *display, uint32_t *seen, const uint8_t *key,
                         size_t key_length, const uint8_t *v, size_t n)
{
    if (is(key, key_length, "desktop")) {
        const uint8_t *x = memchr(v, 'x', n);
        uint32_t w, h;
        if ((*seen & 1u) || !x || number(v, (size_t)(x - v), &w) != PW_OK ||
            number(x + 1, n - (size_t)(x - v) - 1, &h) != PW_OK ||
            w < PW_GAME_DESKTOP_MIN_W || h < PW_GAME_DESKTOP_MIN_H ||
            w > PW_GAME_DESKTOP_MAX_W || h > PW_GAME_DESKTOP_MAX_H)
            return PW_ERR_MALFORMED;
        display->width = w;
        display->height = h;
        *seen |= 1u;
        return PW_OK;
    }
    if (is(key, key_length, "view")) {
        if (*seen & 4u) return PW_ERR_MALFORMED;
        if (is(v, n, "window")) display->view = PW_GAME_VIEW_WINDOW;
        else if (is(v, n, "desktop")) display->view = PW_GAME_VIEW_DESKTOP;
        else return PW_ERR_UNSUPPORTED;
        *seen |= 4u;
        return PW_OK;
    }
    if (is(key, key_length, "scaling")) {
        if (*seen & 2u) return PW_ERR_MALFORMED;
        if (is(v, n, "fit")) display->scaling = PW_GAME_SCALING_FIT;
        else if (is(v, n, "integer")) display->scaling = PW_GAME_SCALING_INTEGER;
        else if (is(v, n, "stretch")) display->scaling = PW_GAME_SCALING_STRETCH;
        else return PW_ERR_UNSUPPORTED;
        *seen |= 2u;
        return PW_OK;
    }
    if (is(key, key_length, "show_fps")) {
        if (*seen & 8u) return PW_ERR_MALFORMED;
        if (is(v, n, "true")) display->show_fps = 1;
        else if (is(v, n, "false")) display->show_fps = 0;
        else return PW_ERR_UNSUPPORTED;
        *seen |= 8u;
        return PW_OK;
    }
    if (is(key, key_length, "refresh")) {
        if (*seen & 16u) return PW_ERR_MALFORMED;
        if (is(v, n, "60")) display->refresh = 60;
        else if (is(v, n, "120")) display->refresh = 120;
        else return PW_ERR_UNSUPPORTED;
        *seen |= 16u;
        return PW_OK;
    }
    if (is(key, key_length, "opengl_thread")) {
        if (*seen & 32u) return PW_ERR_MALFORMED;
        if (is(v, n, "true")) display->opengl_thread = 1;
        else if (is(v, n, "false")) display->opengl_thread = 0;
        else return PW_ERR_UNSUPPORTED;
        *seen |= 32u;
        return PW_OK;
    }
    return PW_ERR_UNSUPPORTED;
}

enum { SECTION_NONE, SECTION_APPLICATION, SECTION_DISPLAY, SECTION_INPUT, SECTION_DEBUG, SECTION_RUNTIME };

/* One line: its trimmed extent and where the next begins. */
static size_t next_line(const uint8_t *bytes, size_t length, size_t cursor,
                        const uint8_t **begin, const uint8_t **end)
{
    size_t start = cursor;

    while (cursor < length && bytes[cursor] != '\n' && bytes[cursor] != '\r') cursor++;
    *begin = bytes + start;
    *end = bytes + cursor;
    trim(begin, end);
    while (cursor < length && (bytes[cursor] == '\n' || bytes[cursor] == '\r')) cursor++;
    return cursor;
}

static int section_of(const uint8_t *begin, const uint8_t *end)
{
    if (end - begin < 3 || end[-1] != ']') return -1;
    begin++, end--;
    return is(begin, (size_t)(end - begin), "application") ? SECTION_APPLICATION :
           is(begin, (size_t)(end - begin), "display") ? SECTION_DISPLAY :
           is(begin, (size_t)(end - begin), "input") ? SECTION_INPUT :
           is(begin, (size_t)(end - begin), "debug") ? SECTION_DEBUG :
           is(begin, (size_t)(end - begin), "runtime") ? SECTION_RUNTIME : -1;
}

/* [debug] winedebug = Wine's channel list (e.g. +seh,warn+module,-all):
 * letters, digits and _ + - , = . only, so nothing but a WINEDEBUG value
 * reaches Wine's environment. */
static int env_value_char(uint8_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '+' || c == '-' || c == ',' || c == '.' || c == '=' || c == ':' || c == '/';
}

static int starts(const uint8_t *text, size_t length, const char *prefix)
{
    size_t n = strlen(prefix);
    return length >= n && !memcmp(text, prefix, n);
}

static int same(const uint8_t *text, size_t length, const char *name)
{
    return strlen(name) == length && !memcmp(text, name, length);
}

/* [debug] env = NAME=VALUE (src/pw_game_profile.h): only the families of
 * variables a game's graphics and the native CPU read, never one the title
 * sets itself, so a profile adds diagnostics but cannot change what the
 * title decides. DXVK_HUD/GALLIUM_HUD are checked against show_fps after
 * the whole profile is read. */
static int debug_env_field(PwGameProfile *profile, const uint8_t *value, size_t value_length)
{
    static const char *const families[] = { "PW_", "DXVK_", "MESA_", "GALLIUM_", "ZINK_", "RADV_", "VK_" };
    static const char *const title_set[] = { "PW_VK_BATCH", "PW_INPUT_SHARED_FAST", "GALLIUM_DRIVER" };
    const uint8_t *equals = memchr(value, '=', value_length);
    size_t name_length, data_length;
    PwGameDebugEnv *entry;
    int family = 0;

    if (!equals || profile->debug_env_count >= PW_GAME_DEBUG_ENV_MAX) return PW_ERR_MALFORMED;
    name_length = (size_t)(equals - value);
    data_length = value_length - name_length - 1;
    if (!name_length || name_length >= PW_GAME_DEBUG_ENV_NAME ||
        !data_length || data_length >= PW_GAME_DEBUG_ENV_VALUE)
        return PW_ERR_MALFORMED;
    for (size_t i = 0; i < name_length; i++) {
        uint8_t c = value[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return PW_ERR_MALFORMED;
    }
    for (size_t i = 0; i < sizeof(families) / sizeof(families[0]); i++)
        family |= starts(value, name_length, families[i]);
    if (!family || starts(value, name_length, "PW_QPC_TSC_")) return PW_ERR_UNSUPPORTED;
    for (size_t i = 0; i < sizeof(title_set) / sizeof(title_set[0]); i++)
        if (same(value, name_length, title_set[i])) return PW_ERR_UNSUPPORTED;
    for (size_t i = 0; i < data_length; i++)
        if (!env_value_char(equals[1 + i])) return PW_ERR_MALFORMED;
    for (size_t i = 0; i < profile->debug_env_count; i++)
        if (same(value, name_length, profile->debug_env[i].name)) return PW_ERR_MALFORMED;
    entry = &profile->debug_env[profile->debug_env_count++];
    memcpy(entry->name, value, name_length);
    entry->name[name_length] = 0;
    memcpy(entry->value, equals + 1, data_length);
    entry->value[data_length] = 0;
    return PW_OK;
}

static int debug_field(PwGameProfile *profile, const uint8_t *key, size_t key_length,
                       const uint8_t *value, size_t value_length)
{
    if (is(key, key_length, "env")) return debug_env_field(profile, value, value_length);
    if (!is(key, key_length, "winedebug")) return PW_ERR_MALFORMED;
    if (!value_length || value_length >= sizeof(profile->winedebug) || profile->winedebug[0])
        return PW_ERR_MALFORMED;
    for (size_t i = 0; i < value_length; i++) {
        uint8_t c = value[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '+' || c == '-' || c == ',' || c == '=' || c == '.'))
            return PW_ERR_MALFORMED;
    }
    memcpy(profile->winedebug, value, value_length);
    profile->winedebug[value_length] = 0;
    return PW_OK;
}

/* [runtime] thread_scheduling, shared_input, fast_clock: true/false or
 * 1/0; cpu: native or translator; each once. */
static int runtime_field(PwGameRuntime *runtime, uint32_t *seen, const uint8_t *key, size_t key_length,
                         const uint8_t *v, size_t n)
{
    uint32_t bit;
    int *field, on;

    if (is(key, key_length, "cpu")) {
        if (*seen & 2u) return PW_ERR_MALFORMED;
        if (is(v, n, "native")) runtime->cpu = PW_GAME_CPU_NATIVE;
        else if (is(v, n, "translator")) runtime->cpu = PW_GAME_CPU_TRANSLATOR;
        else return PW_ERR_UNSUPPORTED;
        *seen |= 2u;
        return PW_OK;
    }
    if (is(key, key_length, "thread_scheduling")) bit = 1u, field = &runtime->thread_scheduling;
    else if (is(key, key_length, "shared_input")) bit = 4u, field = &runtime->shared_input;
    else if (is(key, key_length, "fast_clock")) bit = 8u, field = &runtime->fast_clock;
    else return PW_ERR_UNSUPPORTED;
    if (*seen & bit) return PW_ERR_MALFORMED;
    if (is(v, n, "true") || is(v, n, "1")) on = 1;
    else if (is(v, n, "false") || is(v, n, "0")) on = 0;
    else return PW_ERR_UNSUPPORTED;
    *field = on;
    *seen |= bit;
    return PW_OK;
}

/* key = value lines of the display/input sections; application lines are
 * left to pw_app_profile. */
static int parse_sections(const uint8_t *bytes, size_t length, PwGameProfile *profile,
                          PwGameInput *input_only, size_t *application_end)
{
    int section = SECTION_NONE;
    uint32_t seen_sections = 0, display_seen = 0, runtime_seen = 0;
    size_t cursor = 0;

    while (cursor < length) {
        const uint8_t *begin, *end, *equals, *key_end, *value;
        size_t line = cursor;

        cursor = next_line(bytes, length, cursor, &begin, &end);
        if (begin == end || *begin == ';' || *begin == '#') continue;
        if (*begin == '[') {
            int next = section_of(begin, end);
            if (next < 0 || (seen_sections & (1u << next))) return PW_ERR_MALFORMED;
            if (input_only ? next != SECTION_INPUT :
                (section == SECTION_NONE) != (next == SECTION_APPLICATION))
                return PW_ERR_MALFORMED;
            if (section == SECTION_APPLICATION && application_end) *application_end = line;
            seen_sections |= 1u << next;
            section = next;
            continue;
        }
        if (section == SECTION_NONE) return PW_ERR_MALFORMED;
        if (section == SECTION_APPLICATION) continue;
        for (equals = begin; equals < end && *equals != '='; equals++) {}
        if (equals == end) return PW_ERR_MALFORMED;
        key_end = equals;
        trim(&begin, &key_end);
        value = equals + 1;
        trim(&value, &end);
        if (begin == key_end) return PW_ERR_MALFORMED;
        int status = section == SECTION_DEBUG ?
            debug_field(profile, begin, (size_t)(key_end - begin), value, (size_t)(end - value)) :
            section == SECTION_RUNTIME ?
            runtime_field(&profile->runtime, &runtime_seen, begin, (size_t)(key_end - begin),
                          value, (size_t)(end - value)) :
            section == SECTION_DISPLAY ?
            display_field(&profile->display, &display_seen, begin, (size_t)(key_end - begin),
                          value, (size_t)(end - value)) :
            input_field(input_only ? input_only : &profile->input, begin,
                        (size_t)(key_end - begin), value, (size_t)(end - value), !input_only);
        if (status != PW_OK) return status;
    }
    if (input_only ? !(seen_sections & (1u << SECTION_INPUT)) :
        !(seen_sections & (1u << SECTION_APPLICATION)))
        return PW_ERR_MALFORMED;
    return PW_OK;
}

size_t pw_game_runtime_env(const PwGameRuntime *runtime, PwGameEnv *env)
{
    size_t count = 0;

    if (!runtime || !env) return 0;
    if (runtime->thread_scheduling) env[count++] = (PwGameEnv){ "WINE_PS5_SCHED", "1" };
    if (runtime->shared_input) env[count++] = (PwGameEnv){ "PW_INPUT_SHARED_FAST", "1" };
    return count;
}

size_t pw_game_debug_env(const PwGameProfile *profile, PwGameEnv *env)
{
    if (!profile || !env) return 0;
    for (size_t i = 0; i < profile->debug_env_count; i++)
        env[i] = (PwGameEnv){ profile->debug_env[i].name, profile->debug_env[i].value };
    return profile->debug_env_count;
}

int pw_game_cpu_native(const PwGameProfile *profile)
{
    if (!profile || profile->app.architecture != PW_APP_ARCH_PE32) return 0;
    if (profile->runtime.cpu == PW_GAME_CPU_NATIVE) return 1;
    if (profile->runtime.cpu == PW_GAME_CPU_TRANSLATOR) return 0;
    return 1;
}

size_t pw_game_cpu_env(const PwGameProfile *profile, PwGameEnv *env)
{
    if (!env || !pw_game_cpu_native(profile)) return 0;
    /* patch 0611; the native CPU never runs without the batching */
    env[0] = (PwGameEnv){ "WINE_PS5_WOW64_CPU", "wow64native.dll" };
    env[1] = (PwGameEnv){ "PW_VK_BATCH", "1" };
    return 2;
}

size_t pw_game_graphics_env(const PwGameProfile *profile, PwGameEnv *env)
{
    size_t count = 0;
    int zink;
    if (!profile || !env) return 0;
    zink = profile->app.graphics == PW_APP_GRAPHICS_ZINK;
    if (zink) {
        int opted = 0;
        env[count++] = (PwGameEnv){ "GALLIUM_DRIVER", "zink" };
        /* Zink writes its descriptors into a mapped descriptor buffer and never
         * reads them back, so winevulkan may defer vkGetDescriptorEXT (99% of
         * its crossings in Counter-Strike 1.6). [debug] env = PW_VK_DEFER_DESCRIPTORS=0
         * opts out. */
        for (size_t i = 0; i < profile->debug_env_count; i++)
            opted |= !strcmp(profile->debug_env[i].name, "PW_VK_DEFER_DESCRIPTORS");
        if (!opted) env[count++] = (PwGameEnv){ "PW_VK_DEFER_DESCRIPTORS", "1" };
    }
    if (profile->display.show_fps)
        env[count++] = zink ? (PwGameEnv){ "GALLIUM_HUD", "simple,fps" } : (PwGameEnv){ "DXVK_HUD", "fps" };
    return count;
}

void pw_game_input_init(PwGameInput *input)
{
    if (!input) return;
    memset(input, 0, sizeof(*input));
    input->mouse_speed = PW_GAME_MOUSE_SPEED_DEFAULT;
    for (size_t i = 0; i < PW_GAME_BUTTON_COUNT; i++) input->bindings[i].mask = buttons[i].mask;
}

int pw_game_profile_parse(const uint8_t *bytes, size_t length, PwGameProfile *profile)
{
    PwGameProfile parsed;
    size_t application_end = length;
    int status;

    if (!bytes || !profile || !length) return PW_ERR_PRECONDITION;
    if (length > PW_APP_PROFILE_MAX_BYTES) return PW_ERR_LIMIT;
    memset(&parsed, 0, sizeof(parsed));
    pw_game_input_init(&parsed.input);
    parsed.input.mouse_speed = 0;       /* unset until a line sets it */
    parsed.display.show_fps = 1;        /* on unless the profile says false */
    parsed.display.refresh = 60;
    if ((status = parse_sections(bytes, length, &parsed, NULL, &application_end)) != PW_OK)
        return status;
    if ((status = pw_app_profile_parse(bytes, application_end, &parsed.app)) != PW_OK)
        return status;
    /* show_fps sets the HUD variable itself (pw_game_graphics_env). */
    for (size_t i = 0; parsed.display.show_fps && i < parsed.debug_env_count; i++)
        if (!strcmp(parsed.debug_env[i].name, "DXVK_HUD") || !strcmp(parsed.debug_env[i].name, "GALLIUM_HUD"))
            return PW_ERR_UNSUPPORTED;
    *profile = parsed;
    return PW_OK;
}

int pw_game_input_parse(const uint8_t *bytes, size_t length, PwGameInput *input)
{
    PwGameProfile unused;
    PwGameInput parsed;
    int status;

    if (!bytes || !input || !length) return PW_ERR_PRECONDITION;
    if (length > PW_APP_PROFILE_MAX_BYTES) return PW_ERR_LIMIT;
    memset(&unused, 0, sizeof(unused));
    pw_game_input_init(&parsed);
    parsed.mouse_speed = 0;
    if ((status = parse_sections(bytes, length, &unused, &parsed, NULL)) != PW_OK) return status;
    pw_game_input_overlay(input, &parsed);
    return PW_OK;
}

void pw_game_input_overlay(PwGameInput *base, const PwGameInput *overrides)
{
    if (!base || !overrides) return;
    if (overrides->set & SET_MODE) base->mode = overrides->mode;
    if (overrides->set & SET_MOUSE) base->mouse = overrides->mouse;
    if (overrides->set & SET_SPEED) base->mouse_speed = overrides->mouse_speed;
    base->set |= overrides->set & (SET_MODE | SET_MOUSE | SET_SPEED);
    for (size_t i = 0; i < PW_GAME_BUTTON_COUNT; i++)
        if (overrides->bindings[i].kind != PW_GAME_BIND_UNSET) base->bindings[i] = overrides->bindings[i];
}

void pw_game_input_default_mode(PwGameInput *input)
{
    if (!input || (input->set & SET_MODE) || input->mouse != PW_GAME_STICK_NONE) return;
    for (size_t i = 0; i < PW_GAME_BUTTON_COUNT; i++)
        if (input->bindings[i].kind != PW_GAME_BIND_UNSET) return;
    input->mode = PW_GAME_INPUT_XINPUT;
}

uint32_t pw_game_button_mask(size_t index)
{
    return index < PW_GAME_BUTTON_COUNT ? buttons[index].mask : 0u;
}
