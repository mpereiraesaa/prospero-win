/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_launcher_model.h"
#include "../src/pw_prefix_registry.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { FAKE_HIVE_BYTES = 64, FAKE_PREFIXES = 2 };

typedef struct FakeHive {
    char root[PW_PREFIX_PATH_CAPACITY];
    const char *name;
    uint8_t bytes[FAKE_HIVE_BYTES];
    size_t size;
    int present;
} FakeHive;

typedef struct FakeFiles {
    FakeHive hives[FAKE_PREFIXES * PW_WINE_PREFIX_HIVE_COUNT];
    int fail_next_read;
    int fail_next_replace;
    int fail_next_set;
} FakeFiles;

static FakeHive *find_hive(FakeFiles *files, const char *root,
                           const char *name)
{
    for (unsigned index = 0u;
         index < FAKE_PREFIXES * PW_WINE_PREFIX_HIVE_COUNT; ++index) {
        FakeHive *hive = &files->hives[index];
        if (strcmp(hive->root, root) == 0 && strcmp(hive->name, name) == 0)
            return hive;
    }
    return NULL;
}

static int fake_read(void *context, const char *root, const char *name,
                     uint8_t *output, size_t capacity, size_t *written)
{
    FakeFiles *files = context;
    FakeHive *hive = find_hive(context, root, name);

    if (files->fail_next_read) {
        files->fail_next_read = 0;
        return PW_ERR_VM;
    }
    if (!hive || !hive->present)
        return PW_ERR_NOT_FOUND;
    if (hive->size > capacity)
        return PW_ERR_LIMIT;
    memcpy(output, hive->bytes, hive->size);
    *written = hive->size;
    return PW_OK;
}

static int fake_replace(void *context, const char *root, const char *name,
                        const uint8_t *bytes, size_t size)
{
    FakeFiles *files = context;
    FakeHive *hive = find_hive(files, root, name);

    if (files->fail_next_replace) {
        files->fail_next_replace = 0;
        return PW_ERR_VM;
    }
    if (!hive) {
        for (unsigned index = 0u;
             index < FAKE_PREFIXES * PW_WINE_PREFIX_HIVE_COUNT; ++index) {
            if (files->hives[index].name)
                continue;
            hive = &files->hives[index];
            strcpy(hive->root, root);
            hive->name = name;
            break;
        }
    }
    if (!hive || size > sizeof(hive->bytes))
        return PW_ERR_LIMIT;
    if (size)
        memcpy(hive->bytes, bytes, size);
    hive->size = size;
    hive->present = 1;
    return PW_OK;
}

static int fake_replace_hive_set(
    void *context, const char *root,
    const char *const names[PW_WINE_PREFIX_HIVE_COUNT],
    const uint8_t *const bytes[PW_WINE_PREFIX_HIVE_COUNT],
    const size_t sizes[PW_WINE_PREFIX_HIVE_COUNT])
{
    FakeFiles *files = context;
    FakeFiles staged;

    if (files->fail_next_set) {
        files->fail_next_set = 0;
        return PW_ERR_VM;
    }
    staged = *files;
    for (size_t index = 0u; index < PW_WINE_PREFIX_HIVE_COUNT; ++index) {
        const int result = fake_replace(&staged, root, names[index],
                                        bytes[index], sizes[index]);
        if (result != PW_OK)
            return result;
    }
    memcpy(files->hives, staged.hives, sizeof(files->hives));
    return PW_OK;
}

static void make_prefix(PwPrefixLayout *prefix, const char *id)
{
    memset(prefix, 0, sizeof(*prefix));
    assert(strlen(id) < sizeof(prefix->id));
    strcpy(prefix->id, id);
    assert(snprintf(prefix->root, sizeof(prefix->root),
                    "/data/prospero-win/prefixes/%s", id) > 0);
    assert(snprintf(prefix->hives[PW_PREFIX_HIVE_SYSTEM],
                    sizeof(prefix->hives[PW_PREFIX_HIVE_SYSTEM]),
                    "%s/system.reg", prefix->root) > 0);
    assert(snprintf(prefix->hives[PW_PREFIX_HIVE_USER],
                    sizeof(prefix->hives[PW_PREFIX_HIVE_USER]),
                    "%s/user.reg", prefix->root) > 0);
    assert(snprintf(prefix->drive_c, sizeof(prefix->drive_c),
                    "%s/drive_c", prefix->root) > 0);
}

static void test_registry_hive_storage(void)
{
    FakeFiles files = {0};
    PwPrefixRegistryStore store;
    PwPrefixRegistryIo io = {
        .context = &files,
        .read_file = fake_read,
        .replace_file = fake_replace,
        .replace_hive_set = fake_replace_hive_set,
    };
    PwPrefixLayout pinball, paint, invalid;
    const uint8_t wine_text[] = "WINE REGISTRY Version 2\n[Software\\Vendor]\n";
    uint8_t output[FAKE_HIVE_BYTES];
    size_t written = 77u;

    make_prefix(&pinball, "pinball");
    make_prefix(&paint, "paint");
    assert(pw_prefix_registry_store_init(&store, &io) == PW_OK);
    assert(pw_prefix_registry_store_init(&store, NULL) == PW_ERR_PRECONDITION);

    memset(output, 0xa5, sizeof(output));
    assert(pw_prefix_registry_load(&store, &pinball, PW_WINE_PREFIX_HIVE_USER,
                                   output, sizeof(output), &written) ==
           PW_ERR_NOT_FOUND);
    assert(written == 77u && output[0] == 0xa5u);

    assert(pw_prefix_registry_save(&store, &pinball, PW_WINE_PREFIX_HIVE_USER,
                                   wine_text, sizeof(wine_text)) == PW_OK);
    assert(pw_prefix_registry_save(&store, &pinball,
                                   PW_WINE_PREFIX_HIVE_SYSTEM,
                                   wine_text, sizeof(wine_text) - 2u) == PW_OK);
    assert(pw_prefix_registry_save(&store, &pinball,
                                   PW_WINE_PREFIX_HIVE_USERDEF,
                                   wine_text, sizeof(wine_text) - 1u) == PW_OK);
    assert(find_hive(&files, pinball.root, "system.reg") != NULL);
    assert(find_hive(&files, pinball.root, "userdef.reg") != NULL);
    assert(find_hive(&files, pinball.root, "user.reg") != NULL);
    assert(find_hive(&files, pinball.root, "classes.reg") == NULL);
    assert(pw_prefix_registry_load(&store, &pinball, PW_WINE_PREFIX_HIVE_USER,
                                   output, sizeof(output), &written) == PW_OK);
    assert(written == sizeof(wine_text));
    assert(memcmp(output, wine_text, sizeof(wine_text)) == 0);
    assert(pw_prefix_registry_load(&store, &paint, PW_WINE_PREFIX_HIVE_USER,
                                   output, sizeof(output), &written) ==
           PW_ERR_NOT_FOUND);

    /* A failed backend read/write must preserve the caller's old state and
     * the last complete hive; each operation is explicitly prefix-scoped. */
    memset(output, 0x5a, sizeof(output));
    written = 91u;
    files.fail_next_read = 1;
    assert(pw_prefix_registry_load(&store, &pinball, PW_WINE_PREFIX_HIVE_USER,
                                   output, sizeof(output), &written) == PW_ERR_VM);
    assert(written == 91u && output[0] == 0x5au);
    files.fail_next_replace = 1;
    assert(pw_prefix_registry_save(&store, &pinball, PW_WINE_PREFIX_HIVE_USER,
                                   (const uint8_t *)"partial", 7u) == PW_ERR_VM);
    assert(pw_prefix_registry_load(&store, &pinball, PW_WINE_PREFIX_HIVE_USER,
                                   output, sizeof(output), &written) == PW_OK);
    assert(written == sizeof(wine_text));
    assert(memcmp(output, wine_text, sizeof(wine_text)) == 0);

    assert(pw_prefix_registry_save(&store, &paint, PW_WINE_PREFIX_HIVE_USER,
                                   (const uint8_t *)"paint hive", 10u) == PW_OK);
    assert(pw_prefix_registry_load(&store, &paint, PW_WINE_PREFIX_HIVE_USER,
                                   output, sizeof(output), &written) == PW_OK);
    assert(written == 10u && memcmp(output, "paint hive", 10u) == 0);
    assert(pw_prefix_registry_load(&store, &pinball, PW_WINE_PREFIX_HIVE_USER,
                                   output, sizeof(output), &written) == PW_OK);
    assert(written == sizeof(wine_text));
    assert(memcmp(output, wine_text, sizeof(wine_text)) == 0);

    assert(pw_prefix_registry_load(&store, &pinball,
                                   (PwWinePrefixHive)PW_WINE_PREFIX_HIVE_COUNT,
                                   output, sizeof(output), &written) ==
           PW_ERR_MALFORMED);

    invalid = pinball;
    strcpy(invalid.root, "/data/prospero-win/prefixes/other");
    assert(pw_prefix_registry_load(&store, &invalid, PW_WINE_PREFIX_HIVE_USER,
                                   output, sizeof(output), &written) ==
           PW_ERR_MALFORMED);

    written = 55u;
    assert(pw_prefix_registry_load(&store, &pinball, PW_WINE_PREFIX_HIVE_USER,
                                   output, 1u, &written) == PW_ERR_LIMIT);
    assert(written == 55u);

    {
        const uint8_t system[] = "pinball system";
        const uint8_t userdef[] = "pinball default user";
        const uint8_t user[] = "pinball user";
        const uint8_t next_system[] = "next system";
        const uint8_t next_userdef[] = "next default";
        const uint8_t next_user[] = "next user";
        const uint8_t oversized[FAKE_HIVE_BYTES + 1u] = { 0u };
        const PwWinePrefixHiveData snapshot[PW_WINE_PREFIX_HIVE_COUNT] = {
            { system, sizeof(system) - 1u },
            { userdef, sizeof(userdef) - 1u },
            { user, sizeof(user) - 1u },
        };
        const PwWinePrefixHiveData next_snapshot[PW_WINE_PREFIX_HIVE_COUNT] = {
            { next_system, sizeof(next_system) - 1u },
            { next_userdef, sizeof(next_userdef) - 1u },
            { next_user, sizeof(next_user) - 1u },
        };
        const PwWinePrefixHiveData oversized_snapshot[
            PW_WINE_PREFIX_HIVE_COUNT] = {
                { next_system, sizeof(next_system) - 1u },
                { next_userdef, sizeof(next_userdef) - 1u },
                { oversized, sizeof(oversized) },
            };
        const PwWinePrefixHiveData invalid_snapshot[PW_WINE_PREFIX_HIVE_COUNT] = {
            { next_system, sizeof(next_system) - 1u },
            { NULL, 1u },
            { next_user, sizeof(next_user) - 1u },
        };
        const uint8_t *expected[PW_WINE_PREFIX_HIVE_COUNT] = {
            system, userdef, user,
        };
        const size_t expected_sizes[PW_WINE_PREFIX_HIVE_COUNT] = {
            sizeof(system) - 1u, sizeof(userdef) - 1u, sizeof(user) - 1u,
        };

        assert(pw_prefix_registry_save_snapshot(&store, &pinball, snapshot) ==
               PW_OK);
        for (size_t index = 0u; index < PW_WINE_PREFIX_HIVE_COUNT; ++index) {
            assert(pw_prefix_registry_load(&store, &pinball,
                                           (PwWinePrefixHive)index, output,
                                           sizeof(output), &written) == PW_OK);
            assert(written == expected_sizes[index]);
            assert(memcmp(output, expected[index], written) == 0);
        }
        files.fail_next_set = 1;
        assert(pw_prefix_registry_save_snapshot(&store, &pinball,
                                                next_snapshot) == PW_ERR_VM);
        for (size_t index = 0u; index < PW_WINE_PREFIX_HIVE_COUNT; ++index) {
            assert(pw_prefix_registry_load(&store, &pinball,
                                           (PwWinePrefixHive)index, output,
                                           sizeof(output), &written) == PW_OK);
            assert(written == expected_sizes[index]);
            assert(memcmp(output, expected[index], written) == 0);
        }
        assert(pw_prefix_registry_save_snapshot(&store, &pinball,
                                                oversized_snapshot) ==
               PW_ERR_LIMIT);
        assert(pw_prefix_registry_save_snapshot(&store, &pinball,
                                                invalid_snapshot) ==
               PW_ERR_PRECONDITION);
        for (size_t index = 0u; index < PW_WINE_PREFIX_HIVE_COUNT; ++index) {
            assert(pw_prefix_registry_load(&store, &pinball,
                                           (PwWinePrefixHive)index, output,
                                           sizeof(output), &written) == PW_OK);
            assert(written == expected_sizes[index]);
            assert(memcmp(output, expected[index], written) == 0);
        }
        assert(pw_prefix_registry_load(&store, &paint,
                                      PW_WINE_PREFIX_HIVE_SYSTEM, output,
                                      sizeof(output), &written) == PW_ERR_NOT_FOUND);

        {
            PwPrefixRegistryStore singles_only;
            PwPrefixRegistryIo single_io = {
                .context = &files,
                .read_file = fake_read,
                .replace_file = fake_replace,
            };
            assert(pw_prefix_registry_store_init(&singles_only, &single_io) ==
                   PW_OK);
            assert(pw_prefix_registry_save_snapshot(&singles_only, &pinball,
                                                    next_snapshot) ==
                   PW_ERR_UNSUPPORTED);
            assert(pw_prefix_registry_load(&store, &pinball,
                                           PW_WINE_PREFIX_HIVE_SYSTEM, output,
                                           sizeof(output), &written) == PW_OK);
            assert(written == expected_sizes[PW_WINE_PREFIX_HIVE_SYSTEM]);
            assert(memcmp(output, expected[PW_WINE_PREFIX_HIVE_SYSTEM],
                          written) == 0);
        }
    }
}

static const uint8_t pinball_profile_data[] =
    "[application]\n"
    "id=space-cadet-pinball\n"
    "name=Space Cadet Pinball\n"
    "executable=C:\\Games\\Pinball\\PINBALL.EXE\n"
    "working_directory=C:\\Games\\Pinball\n"
    "prefix=pinball\n"
    "runtime=wine-i386-pinned\n"
    "architecture=pe32\n"
    "graphics=gdi\n";

static const uint8_t paint_profile_data[] =
    "[application]\n"
    "id=paint\n"
    "name=Paint\n"
    "executable=C:\\Windows\\System32\\mspaint.exe\n"
    "working_directory=C:\\Windows\\System32\n"
    "prefix=paint\n"
    "runtime=wine-i386-pinned\n"
    "architecture=pe32\n"
    "graphics=gdi\n";

static void test_launcher_model(void)
{
    PwLauncherModel model;
    PwLauncherView view;
    PwAppProfile pinball, paint;
    const PwAppProfile *listed = NULL;
    PwPrefixLayout pinball_prefix, paint_prefix;

    assert(pw_app_profile_parse(pinball_profile_data,
                                sizeof(pinball_profile_data) - 1u,
                                &pinball) == PW_OK);
    assert(pw_app_profile_parse(paint_profile_data,
                                sizeof(paint_profile_data) - 1u,
                                &paint) == PW_OK);
    make_prefix(&pinball_prefix, "pinball");
    make_prefix(&paint_prefix, "paint");

    pw_launcher_model_init(&model);
    assert(pw_launcher_model_view(&model, &view) == PW_OK);
    assert(view.status == PW_LAUNCHER_STATUS_EMPTY);
    assert(view.selected_profile == NULL && view.active_profile == NULL);
    assert(pw_launcher_model_select(&model, 0u) == PW_ERR_NOT_FOUND);
    assert(pw_launcher_model_profile_at(&model, 0u, &listed) == PW_ERR_NOT_FOUND);

    assert(pw_launcher_model_add(&model, &pinball, &pinball_prefix) == PW_OK);
    assert(pw_launcher_model_add(&model, &paint, &paint_prefix) == PW_OK);
    assert(pw_launcher_model_add(&model, &paint, &paint_prefix) == PW_ERR_MALFORMED);
    assert(model.count == 2u && model.selected_index == 0u);
    assert(pw_launcher_model_profile_at(&model, 0u, &listed) == PW_OK);
    assert(strcmp(listed->id, "space-cadet-pinball") == 0);
    assert(pw_launcher_model_profile_at(&model, 1u, &listed) == PW_OK);
    assert(strcmp(listed->id, "paint") == 0);
    assert(pw_launcher_model_select(&model, 1u) == PW_OK);
    assert(pw_launcher_model_request_launch(&model) == PW_OK);
    assert(pw_launcher_model_view(&model, &view) == PW_OK);
    assert(view.status == PW_LAUNCHER_STATUS_PREPARING);
    assert(strcmp(view.selected_profile->id, "paint") == 0);
    assert(strcmp(view.active_profile->id, "paint") == 0);
    assert(pw_launcher_model_select(&model, 0u) == PW_ERR_STATE);
    assert(pw_launcher_model_add(&model, &pinball, &pinball_prefix) == PW_ERR_STATE);

    assert(pw_launcher_model_guest_started(&model) == PW_OK);
    assert(pw_launcher_model_view(&model, &view) == PW_OK);
    assert(view.status == PW_LAUNCHER_STATUS_RUNNING);
    assert(pw_launcher_model_request_stop(&model) == PW_OK);
    assert(pw_launcher_model_view(&model, &view) == PW_OK);
    assert(view.status == PW_LAUNCHER_STATUS_STOPPING);
    assert(pw_launcher_model_guest_exited(&model, 0) == PW_OK);
    assert(pw_launcher_model_view(&model, &view) == PW_OK);
    assert(view.status == PW_LAUNCHER_STATUS_CLEANUP);
    assert(pw_launcher_model_cleanup_complete(&model, PW_ERR_VM) == PW_ERR_VM);
    assert(pw_launcher_model_view(&model, &view) == PW_OK);
    assert(view.status == PW_LAUNCHER_STATUS_CLEANUP);
    assert(view.active_profile != NULL);

    assert(pw_launcher_model_cleanup_complete(&model, PW_OK) == PW_OK);
    assert(pw_launcher_model_view(&model, &view) == PW_OK);
    assert(view.status == PW_LAUNCHER_STATUS_COMPLETE);
    assert(view.active_profile == NULL && view.last_result != NULL);
    assert(view.last_result->guest_status == 0);
    assert(pw_launcher_model_request_launch(&model) == PW_ERR_STATE);
    assert(pw_launcher_model_select(&model, 0u) == PW_ERR_STATE);
    assert(pw_launcher_model_return_to_catalogue(&model) == PW_OK);
    assert(pw_launcher_model_view(&model, &view) == PW_OK);
    assert(view.status == PW_LAUNCHER_STATUS_CATALOGUE);
    assert(view.selected_profile != NULL && view.last_result == NULL);

    assert(pw_launcher_model_request_launch(&model) == PW_OK);
    assert(pw_launcher_model_fail(&model, PW_ERR_UNSUPPORTED) == PW_OK);
    assert(pw_launcher_model_cleanup_complete(&model, PW_OK) == PW_OK);
    assert(pw_launcher_model_view(&model, &view) == PW_OK);
    assert(view.status == PW_LAUNCHER_STATUS_FAILED);
    assert(pw_launcher_model_return_to_catalogue(&model) == PW_OK);
    assert(pw_launcher_model_view(&model, &view) == PW_OK);
    assert(view.status == PW_LAUNCHER_STATUS_CATALOGUE);
}

int main(void)
{
    test_registry_hive_storage();
    test_launcher_model();
    return 0;
}
