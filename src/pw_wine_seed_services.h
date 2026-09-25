/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PROSPERO_WIN_PW_WINE_SEED_SERVICES_H
#define PROSPERO_WIN_PW_WINE_SEED_SERVICES_H

#include "pw_wine_gate.h"

enum {
    PW_WINE_SEED_MAX_KEYS = 128,
    PW_WINE_SEED_MAX_VALUES = 64,
    PW_WINE_SEED_KEY_PATH_BYTES = 2 * PW_WINE_GATE_MAX_PATH + 1,
    PW_WINE_SEED_VALUE_NAME_BYTES = 128,
};

typedef struct PwWineSeedKey {
    char path[PW_WINE_SEED_KEY_PATH_BYTES];
    uint8_t used;
} PwWineSeedKey;

typedef struct PwWineSeedValue {
    uint16_t key_index;
    char name[PW_WINE_SEED_VALUE_NAME_BYTES];
    uint32_t type;
    uint32_t size;
    uint8_t used;
    uint8_t bytes[PW_WINE_GATE_MAX_VALUE];
} PwWineSeedValue;

/* Small, per-run registry/object defaults for getting pinned ntdll started.
 * This is not a Wine hive parser or a persistence implementation. */
typedef struct PwWineSeedServices {
    PwWineRegistryService registry;
    PwWineObjectService objects;
    PwWineSeedKey keys[PW_WINE_SEED_MAX_KEYS];
    PwWineSeedValue values[PW_WINE_SEED_MAX_VALUES];
    uint32_t key_count;
} PwWineSeedServices;

void pw_wine_seed_services_init(PwWineSeedServices *services);
const PwWineRegistryService *pw_wine_seed_registry(
    const PwWineSeedServices *services);
const PwWineObjectService *pw_wine_seed_objects(
    const PwWineSeedServices *services);
const uint8_t *pw_wine_seed_user_sid(uint32_t *bytes);

#endif
