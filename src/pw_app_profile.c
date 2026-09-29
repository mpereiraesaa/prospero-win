/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_app_profile.h"

#include <string.h>

enum {
    FIELD_ID = 1u << 0,
    FIELD_NAME = 1u << 1,
    FIELD_EXE = 1u << 2,
    FIELD_CWD = 1u << 3,
    FIELD_ARGUMENTS = 1u << 4,
    FIELD_PREFIX = 1u << 5,
    FIELD_RUNTIME = 1u << 6,
    FIELD_ARCH = 1u << 7,
    FIELD_GRAPHICS = 1u << 8,
    FIELD_STARTUP_COMMAND = 1u << 9,
    FIELD_DLL_OVERRIDES = 1u << 10,
    REQUIRED_FIELDS = FIELD_ID | FIELD_NAME | FIELD_EXE | FIELD_CWD |
                      FIELD_PREFIX | FIELD_RUNTIME | FIELD_ARCH |
                      FIELD_GRAPHICS,
};

static unsigned char lower_ascii(unsigned char value)
{
    return value >= 'A' && value <= 'Z'
        ? (unsigned char)(value + ('a' - 'A')) : value;
}

static int equal_ascii(const uint8_t *text, size_t length, const char *wanted)
{
    size_t wanted_length = strlen(wanted);

    if (length != wanted_length)
        return 0;
    for (size_t index = 0; index < length; ++index) {
        if (lower_ascii(text[index]) !=
            lower_ascii((unsigned char)wanted[index]))
            return 0;
    }
    return 1;
}

static void trim(const uint8_t **begin, const uint8_t **end)
{
    while (*begin < *end && (**begin == ' ' || **begin == '\t'))
        ++*begin;
    while (*end > *begin && ((*end)[-1] == ' ' || (*end)[-1] == '\t'))
        --*end;
}

static int copy_value(char *destination, size_t capacity,
                      const uint8_t *begin, const uint8_t *end,
                      int allow_empty)
{
    size_t length = (size_t)(end - begin);

    if ((!allow_empty && length == 0u) || length >= capacity)
        return length >= capacity ? PW_ERR_LIMIT : PW_ERR_MALFORMED;
    for (size_t index = 0; index < length; ++index) {
        if (begin[index] < 0x20u || begin[index] == 0x7fu)
            return PW_ERR_MALFORMED;
    }
    memcpy(destination, begin, length);
    destination[length] = '\0';
    return PW_OK;
}

static int valid_identifier(const char *text)
{
    if (!text[0] || !((text[0] >= 'a' && text[0] <= 'z') ||
                      (text[0] >= '0' && text[0] <= '9')))
        return 0;
    for (size_t index = 1; text[index]; ++index) {
        unsigned char value = (unsigned char)text[index];
        if (!((value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') || value == '-' || value == '_'))
            return 0;
    }
    return 1;
}

static int valid_windows_path(const char *path)
{
    size_t length = strlen(path);
    size_t segment = 3u;

    if (length < 3u ||
        !((path[0] >= 'A' && path[0] <= 'Z') ||
          (path[0] >= 'a' && path[0] <= 'z')) ||
        path[1] != ':' || path[2] != '\\')
        return 0;
    for (size_t index = 3u; index <= length; ++index) {
        if (path[index] == '/' || path[index] == ':' ||
            (path[index] != '\0' && (unsigned char)path[index] < 0x20u))
            return 0;
        if (path[index] == '\\' || path[index] == '\0') {
            size_t segment_length = index - segment;
            if (segment_length == 0u && index != length)
                return 0;
            if (segment_length == 2u && path[segment] == '.' &&
                path[segment + 1u] == '.')
                return 0;
            segment = index + 1u;
        }
    }
    return 1;
}

/* WINEDLLOVERRIDES syntax: DLL names, "," between names, "=" before a load
 * order (n, b, n,b, empty) and ";" between entries. No spaces or paths. */
static int valid_dll_overrides(const char *text)
{
    for (size_t index = 0; text[index]; ++index) {
        unsigned char value = (unsigned char)text[index];
        if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
              (value >= '0' && value <= '9') || value == ',' || value == '=' ||
              value == ';' || value == '.' || value == '_' || value == '-' || value == '*'))
            return 0;
    }
    return 1;
}

static int parse_command_id(const uint8_t *value, size_t length,
                            uint32_t *command_id)
{
    uint32_t parsed = 0u;

    if (!value || !length || !command_id)
        return PW_ERR_MALFORMED;
    for (size_t index = 0; index < length; ++index) {
        if (value[index] < '0' || value[index] > '9')
            return PW_ERR_MALFORMED;
        parsed = parsed * 10u + (uint32_t)(value[index] - '0');
        if (parsed > 0xffffu)
            return PW_ERR_LIMIT;
    }
    *command_id = parsed;
    return PW_OK;
}

static int has_exe_extension(const char *path)
{
    size_t length = strlen(path);

    return length >= 4u && path[length - 4u] == '.' &&
           lower_ascii((unsigned char)path[length - 3u]) == 'e' &&
           lower_ascii((unsigned char)path[length - 2u]) == 'x' &&
           lower_ascii((unsigned char)path[length - 1u]) == 'e';
}

static int parse_field(PwAppProfile *profile, uint32_t *fields,
                       const uint8_t *key, size_t key_length,
                       const uint8_t *value, const uint8_t *value_end)
{
    uint32_t field;
    int status;

#define MATCH_KEY(name, bit) equal_ascii(key, key_length, name) ? (bit) : 0
    field = MATCH_KEY("id", FIELD_ID);
    if (!field) field = MATCH_KEY("name", FIELD_NAME);
    if (!field) field = MATCH_KEY("executable", FIELD_EXE);
    if (!field) field = MATCH_KEY("working_directory", FIELD_CWD);
    if (!field) field = MATCH_KEY("arguments", FIELD_ARGUMENTS);
    if (!field) field = MATCH_KEY("prefix", FIELD_PREFIX);
    if (!field) field = MATCH_KEY("runtime", FIELD_RUNTIME);
    if (!field) field = MATCH_KEY("architecture", FIELD_ARCH);
    if (!field) field = MATCH_KEY("graphics", FIELD_GRAPHICS);
    if (!field) field = MATCH_KEY("startup_command_id", FIELD_STARTUP_COMMAND);
    if (!field) field = MATCH_KEY("dll_overrides", FIELD_DLL_OVERRIDES);
#undef MATCH_KEY
    if (!field)
        return PW_ERR_UNSUPPORTED;
    if ((*fields & field) != 0u)
        return PW_ERR_MALFORMED;

    switch (field) {
    case FIELD_ID:
        status = copy_value(profile->id, sizeof(profile->id), value,
                            value_end, 0);
        break;
    case FIELD_NAME:
        status = copy_value(profile->name, sizeof(profile->name), value,
                            value_end, 0);
        break;
    case FIELD_EXE:
        status = copy_value(profile->executable, sizeof(profile->executable),
                            value, value_end, 0);
        break;
    case FIELD_CWD:
        status = copy_value(profile->working_directory,
                            sizeof(profile->working_directory), value,
                            value_end, 0);
        break;
    case FIELD_ARGUMENTS:
        status = copy_value(profile->arguments, sizeof(profile->arguments),
                            value, value_end, 1);
        break;
    case FIELD_DLL_OVERRIDES:
        status = copy_value(profile->dll_overrides, sizeof(profile->dll_overrides), value,
                            value_end, 0);
        if (status == PW_OK && !valid_dll_overrides(profile->dll_overrides))
            status = PW_ERR_MALFORMED;
        break;
    case FIELD_STARTUP_COMMAND:
        status = parse_command_id(value, (size_t)(value_end - value),
                                  &profile->startup_command_id);
        break;
    case FIELD_PREFIX:
        status = copy_value(profile->prefix, sizeof(profile->prefix), value,
                            value_end, 0);
        break;
    case FIELD_RUNTIME:
        status = copy_value(profile->runtime, sizeof(profile->runtime), value,
                            value_end, 0);
        break;
    case FIELD_ARCH:
        if (equal_ascii(value, (size_t)(value_end - value), "pe32")) {
            profile->architecture = PW_APP_ARCH_PE32;
            status = PW_OK;
        } else if (equal_ascii(value, (size_t)(value_end - value), "pe64")) {
            profile->architecture = PW_APP_ARCH_PE64;
            status = PW_OK;
        } else {
            status = PW_ERR_UNSUPPORTED;
        }
        break;
    case FIELD_GRAPHICS:
        if (equal_ascii(value, (size_t)(value_end - value), "auto")) {
            profile->graphics = PW_APP_GRAPHICS_AUTO;
            status = PW_OK;
        } else if (equal_ascii(value, (size_t)(value_end - value), "gdi")) {
            profile->graphics = PW_APP_GRAPHICS_GDI;
            status = PW_OK;
        } else if (equal_ascii(value, (size_t)(value_end - value), "dxvk")) {
            profile->graphics = PW_APP_GRAPHICS_DXVK;
            status = PW_OK;
        } else if (equal_ascii(value, (size_t)(value_end - value), "opengl")) {
            profile->graphics = PW_APP_GRAPHICS_OPENGL;
            status = PW_OK;
        } else {
            status = PW_ERR_UNSUPPORTED;
        }
        break;
    default:
        return PW_ERR_STATE;
    }
    if (status == PW_OK)
        *fields |= field;
    return status;
}

int pw_app_profile_parse(const uint8_t *bytes, size_t length,
                         PwAppProfile *profile)
{
    PwAppProfile parsed;
    uint32_t fields = 0u;
    size_t cursor = 0u;
    int in_application = 0;
    int saw_application = 0;

    if (!bytes || !profile || length == 0u)
        return PW_ERR_PRECONDITION;
    if (length > PW_APP_PROFILE_MAX_BYTES)
        return PW_ERR_LIMIT;
    memset(&parsed, 0, sizeof(parsed));

    while (cursor < length) {
        size_t start = cursor;
        const uint8_t *begin;
        const uint8_t *end;

        while (cursor < length && bytes[cursor] != '\n' && bytes[cursor] != '\r')
            ++cursor;
        begin = bytes + start;
        end = bytes + cursor;
        trim(&begin, &end);
        while (cursor < length && (bytes[cursor] == '\n' || bytes[cursor] == '\r'))
            ++cursor;
        if (begin == end || *begin == ';' || *begin == '#')
            continue;
        if (*begin == '[') {
            if (end - begin < 3 || end[-1] != ']' || saw_application ||
                !equal_ascii(begin + 1, (size_t)(end - begin - 2), "application"))
                return PW_ERR_MALFORMED;
            saw_application = 1;
            in_application = 1;
            continue;
        }
        if (!in_application)
            return PW_ERR_MALFORMED;
        {
            const uint8_t *equals = begin;
            const uint8_t *key_end;
            const uint8_t *value;
            while (equals < end && *equals != '=')
                ++equals;
            if (equals == end)
                return PW_ERR_MALFORMED;
            key_end = equals;
            trim(&begin, &key_end);
            value = equals + 1;
            trim(&value, &end);
            if (begin == key_end)
                return PW_ERR_MALFORMED;
            {
                int status = parse_field(&parsed, &fields, begin,
                                         (size_t)(key_end - begin), value, end);
                if (status != PW_OK)
                    return status;
            }
        }
    }
    if (!saw_application || (fields & REQUIRED_FIELDS) != REQUIRED_FIELDS)
        return PW_ERR_MALFORMED;
    if (!valid_identifier(parsed.id) || !valid_identifier(parsed.prefix) ||
        !valid_identifier(parsed.runtime) ||
        !valid_windows_path(parsed.executable) ||
        !valid_windows_path(parsed.working_directory) ||
        !has_exe_extension(parsed.executable))
        return PW_ERR_MALFORMED;
    *profile = parsed;
    return PW_OK;
}
