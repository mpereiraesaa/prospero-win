/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_prefix.h"

#include <string.h>

static int valid_identifier(const char *text)
{
    size_t length = 0u;

    if (!text)
        return 0;
    while (length < PW_PREFIX_ID_CAPACITY && text[length])
        ++length;
    if (length == 0u || length == PW_PREFIX_ID_CAPACITY ||
        !((text[0] >= 'a' && text[0] <= 'z') ||
          (text[0] >= '0' && text[0] <= '9')))
        return 0;
    for (size_t index = 1u; index < length; ++index) {
        char value = text[index];
        if (!((value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') || value == '-' || value == '_'))
            return 0;
    }
    return 1;
}

static int valid_root(const char *root)
{
    size_t length;
    size_t segment = 1u;

    if (!root || root[0] != '/')
        return 0;
    length = strlen(root);
    if (length == 0u || length >= PW_PREFIX_PATH_CAPACITY)
        return 0;
    if (length > 1u && root[length - 1u] == '/')
        return 0;
    for (size_t index = 1u; index <= length; ++index) {
        if (root[index] == '\\' ||
            (root[index] != '\0' && (unsigned char)root[index] < 0x20u))
            return 0;
        if (root[index] == '/' || root[index] == '\0') {
            size_t bytes = index - segment;
            if (bytes == 2u && root[segment] == '.' && root[segment + 1u] == '.')
                return 0;
            if (bytes == 0u && index != length)
                return 0;
            segment = index + 1u;
        }
    }
    return 1;
}

static int child_path(char *output, size_t capacity,
                      const char *parent, const char *child)
{
    size_t parent_length = strlen(parent);
    size_t child_length = strlen(child);
    int separator = parent_length != 1u || parent[0] != '/';
    size_t needed = parent_length + (size_t)separator + child_length + 1u;

    if (needed > capacity)
        return PW_ERR_LIMIT;
    memcpy(output, parent, parent_length);
    if (separator)
        output[parent_length++] = '/';
    memcpy(output + parent_length, child, child_length);
    output[parent_length + child_length] = '\0';
    return PW_OK;
}

static int join_root(char *output, size_t capacity,
                     const char *root, const char *tail)
{
    char base[PW_PREFIX_PATH_CAPACITY];

    if (child_path(base, sizeof(base), root, "prefixes") != PW_OK)
        return PW_ERR_LIMIT;
    return child_path(output, capacity, base, tail);
}

static int build_layout(const PwPrefixService *service, const char *id,
                        PwPrefixLayout *layout)
{
    static const char *const hive_names[PW_PREFIX_HIVE_COUNT] = {
        "system.reg", "user.reg", "userdef.reg", "classes.reg",
    };
    int status;

    memset(layout, 0, sizeof(*layout));
    memcpy(layout->id, id, strlen(id) + 1u);
    status = join_root(layout->root, sizeof(layout->root),
                       service->storage_root, id);
    if (status != PW_OK)
        return status;
#define CHILD(field, parent, name) \
    do { \
        status = child_path(layout->field, sizeof(layout->field), \
                            layout->parent, name); \
        if (status != PW_OK) return status; \
    } while (0)
    CHILD(drive_c, root, "drive_c");
    CHILD(windows, drive_c, "windows");
    CHILD(system32, windows, "system32");
    CHILD(system, windows, "system");
    CHILD(program_files, drive_c, "Program Files");
    CHILD(program_files_x86, drive_c, "Program Files (x86)");
    CHILD(users, drive_c, "users");
    CHILD(user_home, users, "default");
    CHILD(temp, user_home, "AppData/Local/Temp");
#undef CHILD
    for (uint32_t index = 0u; index < PW_PREFIX_HIVE_COUNT; ++index) {
        status = child_path(layout->hives[index], sizeof(layout->hives[index]),
                            layout->root, hive_names[index]);
        if (status != PW_OK)
            return status;
    }
    return PW_OK;
}

int pw_prefix_service_init(PwPrefixService *service, const char *storage_root,
                           const PwPrefixIo *io)
{
    PwPrefixService initialized;
    size_t length;

    if (!service || !storage_root || !io || !io->make_directories)
        return PW_ERR_PRECONDITION;
    if (!valid_root(storage_root))
        return PW_ERR_MALFORMED;
    length = strlen(storage_root);
    memset(&initialized, 0, sizeof(initialized));
    memcpy(initialized.storage_root, storage_root, length + 1u);
    initialized.io = *io;
    *service = initialized;
    return PW_OK;
}

int pw_prefix_open(PwPrefixService *service, const char *id,
                   PwPrefixLayout *layout)
{
    PwPrefixLayout opened;
    const char *directory_list[9];
    int status;

    if (!service || !layout || !service->io.make_directories)
        return PW_ERR_PRECONDITION;
    if (!valid_root(service->storage_root))
        return PW_ERR_STATE;
    if (!valid_identifier(id))
        return PW_ERR_MALFORMED;
    status = build_layout(service, id, &opened);
    if (status != PW_OK)
        return status;
    directory_list[0] = opened.root;
    directory_list[1] = opened.drive_c;
    directory_list[2] = opened.windows;
    directory_list[3] = opened.system32;
    directory_list[4] = opened.system;
    directory_list[5] = opened.program_files;
    directory_list[6] = opened.program_files_x86;
    directory_list[7] = opened.users;
    directory_list[8] = opened.temp;
    for (uint32_t index = 0u; index < 9u; ++index) {
        status = service->io.make_directories(service->io.context,
                                              directory_list[index]);
        if (status != PW_OK)
            return status;
    }
    *layout = opened;
    return PW_OK;
}
