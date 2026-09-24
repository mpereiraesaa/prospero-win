/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_prefix.h"

#include <assert.h>
#include <string.h>

typedef struct MockStorage {
    char paths[9][PW_PREFIX_PATH_CAPACITY];
    uint32_t calls;
    uint32_t fail_call;
} MockStorage;

static int make_directories(void *context, const char *path)
{
    MockStorage *storage = context;
    uint32_t index = storage->calls++;

    assert(index < 9u);
    assert(strlen(path) < sizeof(storage->paths[index]));
    strcpy(storage->paths[index], path);
    return storage->fail_call == index + 1u ? PW_ERR_VM : PW_OK;
}

static void make_service(PwPrefixService *service, MockStorage *storage)
{
    const PwPrefixIo io = { .context = storage,
                            .make_directories = make_directories };
    assert(pw_prefix_service_init(service, "/data/prospero-win", &io) == PW_OK);
}

int main(void)
{
    PwPrefixService service;
    PwPrefixLayout layout;
    MockStorage storage = { 0 };

    make_service(&service, &storage);
    memset(&layout, 0, sizeof(layout));
    assert(pw_prefix_open(&service, "space-cadet-pinball", &layout) == PW_OK);
    assert(strcmp(layout.id, "space-cadet-pinball") == 0);
    assert(strcmp(layout.root,
                  "/data/prospero-win/prefixes/space-cadet-pinball") == 0);
    assert(strcmp(layout.drive_c,
                  "/data/prospero-win/prefixes/space-cadet-pinball/drive_c") == 0);
    assert(strcmp(layout.windows,
                  "/data/prospero-win/prefixes/space-cadet-pinball/drive_c/windows") == 0);
    assert(strcmp(layout.system32,
                  "/data/prospero-win/prefixes/space-cadet-pinball/drive_c/windows/system32") == 0);
    assert(strcmp(layout.system,
                  "/data/prospero-win/prefixes/space-cadet-pinball/drive_c/windows/system") == 0);
    assert(strcmp(layout.program_files,
                  "/data/prospero-win/prefixes/space-cadet-pinball/drive_c/Program Files") == 0);
    assert(strcmp(layout.program_files_x86,
                  "/data/prospero-win/prefixes/space-cadet-pinball/drive_c/Program Files (x86)") == 0);
    assert(strcmp(layout.users,
                  "/data/prospero-win/prefixes/space-cadet-pinball/drive_c/users") == 0);
    assert(strcmp(layout.user_home,
                  "/data/prospero-win/prefixes/space-cadet-pinball/drive_c/users/default") == 0);
    assert(strcmp(layout.temp,
                  "/data/prospero-win/prefixes/space-cadet-pinball/drive_c/users/default/AppData/Local/Temp") == 0);
    assert(strcmp(layout.hives[PW_PREFIX_HIVE_SYSTEM],
                  "/data/prospero-win/prefixes/space-cadet-pinball/system.reg") == 0);
    assert(strcmp(layout.hives[PW_PREFIX_HIVE_USER],
                  "/data/prospero-win/prefixes/space-cadet-pinball/user.reg") == 0);
    assert(strcmp(layout.hives[PW_PREFIX_HIVE_USERDEF],
                  "/data/prospero-win/prefixes/space-cadet-pinball/userdef.reg") == 0);
    assert(strcmp(layout.hives[PW_PREFIX_HIVE_CLASSES],
                  "/data/prospero-win/prefixes/space-cadet-pinball/classes.reg") == 0);
    assert(storage.calls == 9u);
    assert(strcmp(storage.paths[0], layout.root) == 0);
    assert(strcmp(storage.paths[8], layout.temp) == 0);

    assert(pw_prefix_open(&service, "../escape", &layout) == PW_ERR_MALFORMED);
    assert(pw_prefix_open(&service, "UpperCase", &layout) == PW_ERR_MALFORMED);
    assert(storage.calls == 9u);

    {
        PwPrefixService untouched;
        memset(&untouched, 0x5a, sizeof(untouched));
        PwPrefixService before = untouched;
        assert(pw_prefix_service_init(&untouched, "relative/path", &service.io) ==
               PW_ERR_MALFORMED);
        assert(memcmp(&untouched, &before, sizeof(untouched)) == 0);
        assert(pw_prefix_service_init(&untouched, "/data/../escape", &service.io) ==
               PW_ERR_MALFORMED);
        assert(pw_prefix_service_init(&untouched, "/data/prospero-win/",
                                      &service.io) == PW_ERR_MALFORMED);
    }

    {
        PwPrefixLayout unchanged;
        PwPrefixLayout before;
        MockStorage failing = { .fail_call = 4u };
        PwPrefixService failing_service;
        make_service(&failing_service, &failing);
        memset(&unchanged, 0x3c, sizeof(unchanged));
        before = unchanged;
        assert(pw_prefix_open(&failing_service, "paint", &unchanged) == PW_ERR_VM);
        assert(memcmp(&unchanged, &before, sizeof(unchanged)) == 0);
        assert(failing.calls == 4u);
    }

    assert(pw_prefix_service_init(NULL, "/data/prospero-win", &service.io) ==
           PW_ERR_PRECONDITION);
    assert(pw_prefix_open(NULL, "paint", &layout) == PW_ERR_PRECONDITION);
    assert(pw_prefix_open(&service, "paint", NULL) == PW_ERR_PRECONDITION);
    {
        PwPrefixService malformed = service;
        malformed.storage_root[0] = '\0';
        assert(pw_prefix_open(&malformed, "paint", &layout) == PW_ERR_STATE);
        assert(storage.calls == 9u);
    }
    return 0;
}
