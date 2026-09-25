/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_wine_seed_services.h"

#include <string.h>

enum { PW_WINE_SEED_REG_DWORD = 4u };

typedef struct PwWineSeedDefault {
    const char *name;
    uint32_t value;
} PwWineSeedDefault;

static const PwWineSeedDefault session_manager_defaults[] = {
    { "criticalsectiontimeout", 0x00278d00u },
    { "globalflag", 0u },
    { "heapdecommitfreeblockthreshold", 0u },
    { "heapdecommittotalfreethreshold", 0u },
    { "heapsegmentcommit", 0u },
    { "heapsegmentreserve", 0u },
};

static const char session_manager_path[] =
    "\\registry\\machine\\system\\currentcontrolset\\control\\session manager";
static const char user_path[] = "\\registry\\user\\s-1-5-21-0-0-0-1000";
static const char known_dlls_path[] = "\\knowndlls";

static const uint8_t user_sid[] = {
    0x01, 0x05,                             /* revision, subauthority count */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x05,     /* SECURITY_NT_AUTHORITY */
    0x15, 0x00, 0x00, 0x00,                 /* SECURITY_NT_NON_UNIQUE, 21 */
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0xe8, 0x03, 0x00, 0x00,                 /* 1000 */
};

static int key_index(PwWineSeedServices *services, const void *token,
                     uint16_t *index)
{
    if (!services || !token || !index)
        return 0;
    for (uint32_t current = 0u; current < services->key_count; ++current) {
        if (services->keys[current].used &&
            token == (const void *)&services->keys[current]) {
            *index = (uint16_t)current;
            return 1;
        }
    }
    return 0;
}

static PwWineRegistryStatus registry_open(void *context, const char *path,
                                          void **token)
{
    PwWineSeedServices *services = context;

    if (!services || !path || !token)
        return PW_WINE_REGISTRY_ERROR;
    for (uint32_t index = 0u; index < services->key_count; ++index) {
        if (services->keys[index].used &&
            strcmp(services->keys[index].path, path) == 0) {
            *token = &services->keys[index];
            return PW_WINE_REGISTRY_OK;
        }
    }
    return PW_WINE_REGISTRY_NOT_FOUND;
}

static PwWineRegistryStatus registry_create(void *context, const char *path,
                                            void **token, uint32_t *created)
{
    PwWineSeedServices *services = context;
    PwWineRegistryStatus status;
    size_t length;

    if (!services || !path || !token || !created)
        return PW_WINE_REGISTRY_ERROR;
    status = registry_open(context, path, token);
    if (status == PW_WINE_REGISTRY_OK) {
        *created = 0u;
        return status;
    }
    if (status != PW_WINE_REGISTRY_NOT_FOUND ||
        services->key_count >= PW_WINE_SEED_MAX_KEYS)
        return PW_WINE_REGISTRY_ERROR;
    length = strlen(path);
    if (length >= sizeof(services->keys[0].path))
        return PW_WINE_REGISTRY_ERROR;
    {
        const uint32_t index = services->key_count++;
        PwWineSeedKey *key = &services->keys[index];

        memcpy(key->path, path, length + 1u);
        key->used = 1u;
        *token = key;
    }
    *created = 1u;
    return PW_WINE_REGISTRY_OK;
}

static PwWineRegistryStatus registry_query(void *context, void *token,
                                           const char *name, uint32_t *type,
                                           const void **bytes, uint32_t *size)
{
    PwWineSeedServices *services = context;
    uint16_t index;

    if (!services || !name || !type || !bytes || !size ||
        !key_index(services, token, &index))
        return PW_WINE_REGISTRY_ERROR;
    for (uint32_t value = 0u; value < PW_WINE_SEED_MAX_VALUES; ++value) {
        const PwWineSeedValue *stored = &services->values[value];

        if (!stored->used || stored->key_index != index ||
            strcmp(stored->name, name) != 0)
            continue;
        *type = stored->type;
        *bytes = stored->bytes;
        *size = stored->size;
        return PW_WINE_REGISTRY_OK;
    }
    if (index == 0u) {
        for (uint32_t value = 0u;
             value < sizeof(session_manager_defaults) /
                         sizeof(session_manager_defaults[0]);
             ++value) {
            if (strcmp(session_manager_defaults[value].name, name) != 0)
                continue;
            *type = PW_WINE_SEED_REG_DWORD;
            *bytes = &session_manager_defaults[value].value;
            *size = sizeof(uint32_t);
            return PW_WINE_REGISTRY_OK;
        }
    }
    return PW_WINE_REGISTRY_NOT_FOUND;
}

static PwWineRegistryStatus registry_set_value(void *context, void *token,
                                               const char *name,
                                               uint32_t type,
                                               const void *bytes,
                                               uint32_t size)
{
    PwWineSeedServices *services = context;
    PwWineSeedValue *free_slot = NULL;
    uint16_t index;
    size_t length;

    if (!services || !name || (!bytes && size != 0u) ||
        size > PW_WINE_GATE_MAX_VALUE || !key_index(services, token, &index))
        return PW_WINE_REGISTRY_ERROR;
    length = strlen(name);
    if (length >= PW_WINE_SEED_VALUE_NAME_BYTES)
        return PW_WINE_REGISTRY_ERROR;
    for (uint32_t current = 0u; current < PW_WINE_SEED_MAX_VALUES; ++current) {
        PwWineSeedValue *value = &services->values[current];

        if (!value->used) {
            if (!free_slot)
                free_slot = value;
            continue;
        }
        if (value->key_index == index && strcmp(value->name, name) == 0) {
            value->type = type;
            value->size = size;
            if (size != 0u)
                memcpy(value->bytes, bytes, size);
            return PW_WINE_REGISTRY_OK;
        }
    }
    if (!free_slot)
        return PW_WINE_REGISTRY_ERROR;
    free_slot->key_index = index;
    memcpy(free_slot->name, name, length + 1u);
    free_slot->type = type;
    free_slot->size = size;
    if (size != 0u)
        memcpy(free_slot->bytes, bytes, size);
    free_slot->used = 1u;
    return PW_WINE_REGISTRY_OK;
}

static void registry_close(void *context, void *token)
{
    (void)context;
    (void)token;
}

static PwWineObjectStatus object_open(void *context, PwWineObjectKind kind,
                                      const char *path, void **token)
{
    (void)context;
    if (!path || !token || kind != PW_WINE_OBJECT_DIRECTORY ||
        strcmp(path, known_dlls_path) != 0)
        return PW_WINE_OBJECT_NOT_FOUND;
    *token = (void *)known_dlls_path;
    return PW_WINE_OBJECT_OK;
}

static void object_close(void *context, void *token)
{
    (void)context;
    (void)token;
}

void pw_wine_seed_services_init(PwWineSeedServices *services)
{
    if (!services)
        return;
    memset(services, 0, sizeof(*services));
    services->key_count = 2u;
    memcpy(services->keys[0].path, session_manager_path,
           sizeof(session_manager_path));
    services->keys[0].used = 1u;
    memcpy(services->keys[1].path, user_path, sizeof(user_path));
    services->keys[1].used = 1u;
    services->registry = (PwWineRegistryService){
        .context = services,
        .open = registry_open,
        .create = registry_create,
        .query = registry_query,
        .set_value = registry_set_value,
        .close = registry_close,
    };
    services->objects = (PwWineObjectService){
        .context = services,
        .open = object_open,
        .close = object_close,
    };
}

const PwWineRegistryService *pw_wine_seed_registry(
    const PwWineSeedServices *services)
{
    return services ? &services->registry : NULL;
}

const PwWineObjectService *pw_wine_seed_objects(
    const PwWineSeedServices *services)
{
    return services ? &services->objects : NULL;
}

const uint8_t *pw_wine_seed_user_sid(uint32_t *bytes)
{
    if (bytes)
        *bytes = (uint32_t)sizeof(user_sid);
    return user_sid;
}
