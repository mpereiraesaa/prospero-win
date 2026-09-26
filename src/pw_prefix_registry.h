/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PROSPERO_WIN_PW_PREFIX_REGISTRY_H
#define PROSPERO_WIN_PW_PREFIX_REGISTRY_H

#include "pw_prefix.h"

#include <stddef.h>
#include <stdint.h>

/* The three persistent files opened by the pinned Wine server. */
typedef enum PwWinePrefixHive {
    PW_WINE_PREFIX_HIVE_SYSTEM = 0,
    PW_WINE_PREFIX_HIVE_USERDEF = 1,
    PW_WINE_PREFIX_HIVE_USER = 2,
    PW_WINE_PREFIX_HIVE_COUNT = 3,
} PwWinePrefixHive;

/*
 * Every operation names its PwPrefixLayout; the store has no mutable/default
 * prefix, so selecting another prefix cannot redirect subsequent calls.
 * The storage provider owns path construction below prefix_root and must
 * scope every operation to that exact root. read_file returns
 * PW_ERR_NOT_FOUND for a hive not created yet and PW_ERR_LIMIT when the
 * caller's buffer is too small. On any non-PW_OK result it must leave output
 * and *written unchanged; on success, *written must not exceed capacity.
 * replace_file must publish either the complete byte span or an error while
 * preserving the prior file on error. Atomic replacement is per file, not
 * across all three hives; callers must not infer a multi-file transaction.
 */
typedef struct PwPrefixRegistryIo {
    void *context;
    int (*read_file)(void *context, const char *prefix_root,
                     const char *file_name, uint8_t *output,
                     size_t capacity, size_t *written);
    int (*replace_file)(void *context, const char *prefix_root,
                        const char *file_name, const uint8_t *bytes,
                        size_t size);
    /* Optional atomic checkpoint operation. It must replace all three named
     * Wine hives as one transaction or preserve the complete prior set. */
    int (*replace_hive_set)(void *context, const char *prefix_root,
                            const char *const file_names[PW_WINE_PREFIX_HIVE_COUNT],
                            const uint8_t *const bytes[PW_WINE_PREFIX_HIVE_COUNT],
                            const size_t sizes[PW_WINE_PREFIX_HIVE_COUNT]);
} PwPrefixRegistryIo;

typedef struct PwWinePrefixHiveData {
    const uint8_t *bytes;
    size_t size;
} PwWinePrefixHiveData;

typedef struct PwPrefixRegistryStore {
    PwPrefixRegistryIo io;
} PwPrefixRegistryStore;

int pw_prefix_registry_store_init(PwPrefixRegistryStore *store,
                                  const PwPrefixRegistryIo *io);

/* Loads/saves opaque bytes of one Wine registry text file. This does not
 * parse Wine's file format, translate PwRegistry, or create missing hives. */
int pw_prefix_registry_load(PwPrefixRegistryStore *store,
                            const PwPrefixLayout *prefix,
                            PwWinePrefixHive hive, uint8_t *output,
                            size_t capacity, size_t *written);
int pw_prefix_registry_save(PwPrefixRegistryStore *store,
                            const PwPrefixLayout *prefix,
                            PwWinePrefixHive hive, const uint8_t *bytes,
                            size_t size);

/* Atomically checkpoints system.reg, userdef.reg and user.reg for one
 * prefix. Returns PW_ERR_UNSUPPORTED when the backend has no set transaction;
 * it never falls back to three independent replacements. */
int pw_prefix_registry_save_snapshot(
    PwPrefixRegistryStore *store, const PwPrefixLayout *prefix,
    const PwWinePrefixHiveData hives[PW_WINE_PREFIX_HIVE_COUNT]);

#endif
