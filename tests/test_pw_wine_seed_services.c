/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_wine_seed_services.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static PwWineSeedServices seed;

int main(void)
{
    const PwWineRegistryService *registry;
    const PwWineObjectService *objects;
    const uint8_t *sid;
    uint32_t sid_bytes = 0u;
    uint32_t type = 0u, size = 0u, created = 0u;
    uint32_t first_empty;
    const void *data = NULL;
    void *key = NULL, *object = NULL;
    uint32_t value = 0u;
    const char *session =
        "\\registry\\machine\\system\\currentcontrolset\\control\\session manager";

    pw_wine_seed_services_init(&seed);
    registry = pw_wine_seed_registry(&seed);
    objects = pw_wine_seed_objects(&seed);
    sid = pw_wine_seed_user_sid(&sid_bytes);
    assert(registry && registry->open && registry->create &&
           registry->query && registry->set_value && registry->close);
    assert(objects && objects->open && objects->close);
    assert(sid && sid_bytes == 28u && sid[0] == 1u && sid[1] == 5u);

    assert(registry->open(registry->context, session, &key) ==
           PW_WINE_REGISTRY_OK);
    assert(registry->query(registry->context, key, "criticalsectiontimeout",
                           &type, &data, &size) == PW_WINE_REGISTRY_OK);
    assert(type == 4u && size == sizeof(value));
    memcpy(&value, data, sizeof(value));
    assert(value == 0x00278d00u);
    assert(registry->query(registry->context, key, "not-in-seed", &type,
                           &data, &size) == PW_WINE_REGISTRY_NOT_FOUND);

    assert(registry->create(registry->context,
                            "\\registry\\user\\test\\software",
                            &key, &created) == PW_WINE_REGISTRY_OK);
    assert(created == 1u);
    assert(registry->create(registry->context,
                            "\\registry\\user\\test\\software",
                            &key, &created) == PW_WINE_REGISTRY_OK);
    assert(created == 0u);
    value = 0x12345678u;
    assert(registry->set_value(registry->context, key, "sample", 4u,
                               &value, sizeof(value)) == PW_WINE_REGISTRY_OK);
    assert(registry->query(registry->context, key, "sample", &type,
                           &data, &size) == PW_WINE_REGISTRY_OK);
    assert(type == 4u && size == sizeof(value));
    memcpy(&value, data, sizeof(value));
    assert(value == 0x12345678u);
    registry->close(registry->context, key);

    assert(objects->open(objects->context, PW_WINE_OBJECT_DIRECTORY,
                         "\\knowndlls", &object) == PW_WINE_OBJECT_OK);
    assert(object != NULL);
    objects->close(objects->context, object);
    assert(objects->open(objects->context, PW_WINE_OBJECT_SECTION,
                         "\\knowndlls\\ntdll.dll", &object) ==
           PW_WINE_OBJECT_NOT_FOUND);

    first_empty = seed.key_count;
    for (uint32_t index = first_empty; index < PW_WINE_SEED_MAX_KEYS;
         ++index) {
        char path[64];

        (void)snprintf(path, sizeof(path), "\\registry\\user\\fill\\k%u",
                       index);
        assert(registry->create(registry->context, path, &key, &created) ==
               PW_WINE_REGISTRY_OK);
    }
    assert(registry->create(registry->context,
                            "\\registry\\user\\overflow", &key,
                            &created) == PW_WINE_REGISTRY_ERROR);
    return 0;
}
