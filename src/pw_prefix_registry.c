/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_prefix_registry.h"

#include <string.h>

static size_t bounded_length(const char *text, size_t capacity)
{
    size_t length = 0u;
    while (length < capacity && text[length])
        ++length;
    return length;
}

static int valid_identifier(const char *text, size_t length)
{
    if (length == 0u ||
        !((text[0] >= 'a' && text[0] <= 'z') ||
          (text[0] >= '0' && text[0] <= '9')))
        return 0;
    for (size_t index = 1u; index < length; ++index) {
        const char value = text[index];
        if (!((value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') || value == '-' || value == '_'))
            return 0;
    }
    return 1;
}

static const char *hive_file_name(PwWinePrefixHive hive)
{
    static const char *const names[PW_WINE_PREFIX_HIVE_COUNT] = {
        "system.reg", "userdef.reg", "user.reg",
    };

    return hive >= PW_WINE_PREFIX_HIVE_SYSTEM &&
           hive < PW_WINE_PREFIX_HIVE_COUNT ? names[hive] : NULL;
}

static int valid_prefix(const PwPrefixLayout *prefix)
{
    size_t id_length;
    size_t root_length;
    size_t segment = 1u;

    if (!prefix)
        return 0;
    id_length = bounded_length(prefix->id, sizeof(prefix->id));
    root_length = bounded_length(prefix->root, sizeof(prefix->root));
    if (id_length == sizeof(prefix->id) ||
        root_length == 0u || root_length == sizeof(prefix->root) ||
        prefix->root[0] != '/')
        return 0;
    if (!valid_identifier(prefix->id, id_length))
        return 0;
    for (size_t index = 1u; index <= root_length; ++index) {
        if (prefix->root[index] == '\\' ||
            (prefix->root[index] != '\0' &&
             (unsigned char)prefix->root[index] < 0x20u))
            return 0;
        if (prefix->root[index] == '/' || prefix->root[index] == '\0') {
            size_t bytes = index - segment;
            if (bytes == 0u ||
                (bytes == 1u && prefix->root[segment] == '.') ||
                (bytes == 2u && prefix->root[segment] == '.' &&
                 prefix->root[segment + 1u] == '.'))
                return 0;
            segment = index + 1u;
        }
    }
    if (prefix->root[root_length - 1u] == '/')
        return 0;
    if (root_length <= id_length || prefix->root[root_length - id_length - 1u] != '/' ||
        memcmp(prefix->root + root_length - id_length, prefix->id, id_length) != 0)
        return 0;
    return 1;
}

int pw_prefix_registry_store_init(PwPrefixRegistryStore *store,
                                  const PwPrefixRegistryIo *io)
{
    if (!store || !io || !io->read_file || !io->replace_file)
        return PW_ERR_PRECONDITION;
    store->io = *io;
    return PW_OK;
}

int pw_prefix_registry_load(PwPrefixRegistryStore *store,
                            const PwPrefixLayout *prefix,
                            PwWinePrefixHive hive, uint8_t *output,
                            size_t capacity, size_t *written)
{
    const char *file_name = hive_file_name(hive);

    if (!store || !output || !written || !store->io.read_file)
        return PW_ERR_PRECONDITION;
    if (!file_name || !valid_prefix(prefix))
        return PW_ERR_MALFORMED;
    return store->io.read_file(store->io.context, prefix->root, file_name,
                               output, capacity, written);
}

int pw_prefix_registry_save(PwPrefixRegistryStore *store,
                            const PwPrefixLayout *prefix,
                            PwWinePrefixHive hive, const uint8_t *bytes,
                            size_t size)
{
    const char *file_name = hive_file_name(hive);

    if (!store || (!bytes && size != 0u) || !store->io.replace_file)
        return PW_ERR_PRECONDITION;
    if (!file_name || !valid_prefix(prefix))
        return PW_ERR_MALFORMED;
    return store->io.replace_file(store->io.context, prefix->root, file_name,
                                  bytes, size);
}

int pw_prefix_registry_save_snapshot(
    PwPrefixRegistryStore *store, const PwPrefixLayout *prefix,
    const PwWinePrefixHiveData hives[PW_WINE_PREFIX_HIVE_COUNT])
{
    static const char *const names[PW_WINE_PREFIX_HIVE_COUNT] = {
        "system.reg", "userdef.reg", "user.reg",
    };
    const uint8_t *bytes[PW_WINE_PREFIX_HIVE_COUNT];
    size_t sizes[PW_WINE_PREFIX_HIVE_COUNT];

    if (!store || !prefix || !hives)
        return PW_ERR_PRECONDITION;
    if (!valid_prefix(prefix))
        return PW_ERR_MALFORMED;
    if (!store->io.replace_hive_set)
        return PW_ERR_UNSUPPORTED;

    for (size_t index = 0u; index < PW_WINE_PREFIX_HIVE_COUNT; ++index) {
        if (!hives[index].bytes && hives[index].size != 0u)
            return PW_ERR_PRECONDITION;
        bytes[index] = hives[index].bytes;
        sizes[index] = hives[index].size;
    }

    return store->io.replace_hive_set(store->io.context, prefix->root, names,
                                      bytes, sizes);
}
