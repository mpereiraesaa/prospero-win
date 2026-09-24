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

int pw_app_profile_stage_name(const PwAppProfile *profile, char *output,
                              size_t capacity)
{
    const char *end;
    const char *basename;
    size_t length;

    if (!profile || !output || capacity == 0u)
        return PW_ERR_PRECONDITION;
    end = memchr(profile->executable, '\0', sizeof(profile->executable));
    if (!end || end == profile->executable)
        return PW_ERR_MALFORMED;
    basename = profile->executable;
    for (const char *cursor = profile->executable; cursor < end; ++cursor)
        if (*cursor == '\\')
            basename = cursor + 1;
    length = (size_t)(end - basename);
    if (length == 0u)
        return PW_ERR_MALFORMED;
    if (length >= capacity)
        return PW_ERR_LIMIT;
    for (size_t index = 0; index < length; ++index)
        output[index] = (char)lower_ascii((unsigned char)basename[index]);
    output[length] = '\0';
    return PW_OK;
}

static int path_char_equal(char left, char right)
{
    if (left == '/' || left == '\\')
        left = '\\';
    if (right == '/' || right == '\\')
        right = '\\';
    return lower_ascii((unsigned char)left) ==
           lower_ascii((unsigned char)right);
}

static int reserved_windows_component(const char *value,size_t length)
{
    size_t stem=0u;
    while(stem<length && value[stem]!='.')stem++;
    if(stem==3u &&
       ((lower_ascii((unsigned char)value[0])=='c' &&
         lower_ascii((unsigned char)value[1])=='o' &&
         lower_ascii((unsigned char)value[2])=='n') ||
        (lower_ascii((unsigned char)value[0])=='p' &&
         lower_ascii((unsigned char)value[1])=='r' &&
         lower_ascii((unsigned char)value[2])=='n') ||
        (lower_ascii((unsigned char)value[0])=='a' &&
         lower_ascii((unsigned char)value[1])=='u' &&
         lower_ascii((unsigned char)value[2])=='x') ||
        (lower_ascii((unsigned char)value[0])=='n' &&
         lower_ascii((unsigned char)value[1])=='u' &&
         lower_ascii((unsigned char)value[2])=='l')))
        return 1;
    if(stem==4u &&
       lower_ascii((unsigned char)value[3])>='1' &&
       lower_ascii((unsigned char)value[3])<='9' &&
       ((lower_ascii((unsigned char)value[0])=='c' &&
         lower_ascii((unsigned char)value[1])=='o' &&
         lower_ascii((unsigned char)value[2])=='m') ||
        (lower_ascii((unsigned char)value[0])=='l' &&
         lower_ascii((unsigned char)value[1])=='p' &&
         lower_ascii((unsigned char)value[2])=='t')))
        return 1;
    return 0;
}

int pw_app_profile_resolve_staged_file(const PwAppProfile *profile,
                                       const char *guest_path, char *output,
                                       size_t capacity)
{
    char staged[PW_APP_PATH_CAPACITY];
    size_t path_length = 0u;
    size_t directory_length;
    size_t directory_prefix;
    size_t relative_offset = 0u;
    size_t relative_length;
    size_t output_length = 4u;
    const char *directory_end;

    if (!profile || !guest_path || !output || !capacity)
        return PW_ERR_PRECONDITION;
    while (path_length < PW_APP_PATH_CAPACITY && guest_path[path_length])
        ++path_length;
    if (path_length == 0u)
        return PW_ERR_MALFORMED;
    if (path_length == PW_APP_PATH_CAPACITY)
        return PW_ERR_LIMIT;
    directory_end = memchr(profile->working_directory, '\0',
                           sizeof(profile->working_directory));
    if (!directory_end || directory_end == profile->working_directory ||
        !valid_windows_path(profile->working_directory))
        return PW_ERR_MALFORMED;
    directory_length = (size_t)(directory_end - profile->working_directory);
    directory_prefix = directory_length;
    while (directory_prefix > 3u &&
           profile->working_directory[directory_prefix - 1u] == '\\')
        --directory_prefix;

    if (path_length >= 2u && guest_path[1] == ':') {
        if (path_length < 3u ||
            (guest_path[2] != '\\' && guest_path[2] != '/'))
            return PW_ERR_UNSUPPORTED;
        if (path_length <= directory_prefix)
            return PW_ERR_UNSUPPORTED;
        for (size_t index = 0; index < directory_prefix; ++index) {
            if (!path_char_equal(guest_path[index],
                                 profile->working_directory[index]))
                return PW_ERR_UNSUPPORTED;
        }
        if (directory_prefix == 3u &&
            profile->working_directory[2] == '\\') {
            relative_offset = directory_prefix;
        } else {
            if (guest_path[directory_prefix] != '\\' &&
                guest_path[directory_prefix] != '/')
                return PW_ERR_UNSUPPORTED;
            relative_offset = directory_prefix + 1u;
        }
    } else {
        if (guest_path[0] == '\\' || guest_path[0] == '/' ||
            strchr(guest_path, ':'))
            return PW_ERR_UNSUPPORTED;
    }

    relative_length = path_length - relative_offset;
    if (relative_length == 0u || relative_length > PW_PATH_MAX)
        return PW_ERR_UNSUPPORTED;
    memcpy(staged,"app/",4u);
    for(size_t position=0u;position<relative_length;) {
        size_t start=position;
        while(position<relative_length && guest_path[relative_offset+position]!='/' &&
              guest_path[relative_offset+position]!='\\')position++;
        size_t component_length=position-start;
        if(!component_length ||
           (component_length==1u && guest_path[relative_offset+start]=='.') ||
           (component_length==2u && guest_path[relative_offset+start]=='.' &&
            guest_path[relative_offset+start+1u]=='.') ||
           guest_path[relative_offset+position-1u]=='.' ||
           guest_path[relative_offset+position-1u]==' ' ||
           reserved_windows_component(guest_path+relative_offset+start,
                                      component_length))
            return PW_ERR_UNSUPPORTED;
        size_t separator=position<relative_length?1u:0u;
        if(output_length+component_length+separator>=sizeof(staged))
            return PW_ERR_LIMIT;
        for(size_t index=0u;index<component_length;index++) {
            unsigned char value=(unsigned char)guest_path[relative_offset+start+index];
            if(value<0x20u || value==0x7fu || value==':' || value=='*' ||
               value=='?' || value=='"' || value=='<' || value=='>' || value=='|')
                return PW_ERR_UNSUPPORTED;
            staged[output_length++]=(char)lower_ascii(value);
        }
        if(position<relative_length) {
            staged[output_length++]='/';
            position++;
            if(position==relative_length)return PW_ERR_UNSUPPORTED;
        }
    }
    staged[output_length]='\0';
    if (output_length >= capacity)
        return PW_ERR_LIMIT;
    memcpy(output, staged, output_length + 1u);
    return PW_OK;
}

int pw_app_profile_build_command_line(const PwAppProfile *profile,
                                      char *output, size_t capacity)
{
    const char *exe_end;
    const char *args_end;
    size_t exe_bytes;
    size_t args_bytes;
    size_t required;
    size_t cursor = 0u;

    if (!profile || !output || capacity == 0u)
        return PW_ERR_PRECONDITION;
    exe_end = memchr(profile->executable, '\0', sizeof(profile->executable));
    args_end = memchr(profile->arguments, '\0', sizeof(profile->arguments));
    if (!exe_end || exe_end == profile->executable || !args_end)
        return PW_ERR_MALFORMED;
    exe_bytes = (size_t)(exe_end - profile->executable);
    args_bytes = (size_t)(args_end - profile->arguments);
    required = exe_bytes + 2u + 1u;
    if (args_bytes != 0u)
        required += args_bytes + 1u;
    if (required > capacity)
        return PW_ERR_LIMIT;

    output[cursor++] = '"';
    memcpy(output + cursor, profile->executable, exe_bytes);
    cursor += exe_bytes;
    output[cursor++] = '"';
    if (args_bytes != 0u) {
        output[cursor++] = ' ';
        memcpy(output + cursor, profile->arguments, args_bytes);
        cursor += args_bytes;
    }
    output[cursor] = '\0';
    return PW_OK;
}
