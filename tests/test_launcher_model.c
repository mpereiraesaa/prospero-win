/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_launcher_model.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t pinball_manifest[] =
    "[application]\n"
    "id=space-cadet-pinball\n"
    "name=Space Cadet Pinball\n"
    "executable=C:\\Games\\Pinball\\PINBALL.EXE\n"
    "working_directory=C:\\Games\\Pinball\n"
    "prefix=pinball\n"
    "runtime=wine-i386-pinned\n"
    "architecture=pe32\n"
    "graphics=gdi\n";

static const uint8_t paint_manifest[] =
    "[application]\n"
    "id=paint\n"
    "name=Paint\n"
    "executable=C:\\Windows\\System32\\mspaint.exe\n"
    "working_directory=C:\\Windows\\System32\n"
    "prefix=paint\n"
    "runtime=wine-i386-pinned\n"
    "architecture=pe32\n"
    "graphics=gdi\n";

static void parse_profiles(PwAppProfile *pinball, PwAppProfile *paint)
{
    assert(pw_app_profile_parse(pinball_manifest, sizeof(pinball_manifest) - 1u,
                                pinball) == PW_OK);
    assert(pw_app_profile_parse(paint_manifest, sizeof(paint_manifest) - 1u,
                                paint) == PW_OK);
}

static void make_prefix(PwPrefixLayout *prefix, const char *id)
{
    memset(prefix, 0, sizeof(*prefix));
    assert(snprintf(prefix->id, sizeof(prefix->id), "%s", id) > 0);
    assert(snprintf(prefix->root, sizeof(prefix->root),
                    "/data/prospero-win/prefixes/%s", id) > 0);
    assert(snprintf(prefix->drive_c, sizeof(prefix->drive_c),
                    "%s/drive_c", prefix->root) > 0);
    assert(snprintf(prefix->hives[PW_PREFIX_HIVE_SYSTEM],
                    sizeof(prefix->hives[PW_PREFIX_HIVE_SYSTEM]),
                    "%s/system.reg", prefix->root) > 0);
    assert(snprintf(prefix->hives[PW_PREFIX_HIVE_USER],
                    sizeof(prefix->hives[PW_PREFIX_HIVE_USER]),
                    "%s/user.reg", prefix->root) > 0);
}

static void test_empty_add_and_enumeration(void)
{
    PwLauncherModel model;
    PwLauncherView view;
    PwAppProfile pinball, paint, wrong_profile;
    PwPrefixLayout pinball_prefix, paint_prefix, wrong_prefix;
    const PwAppProfile *listed = NULL;

    parse_profiles(&pinball, &paint);
    make_prefix(&pinball_prefix, "pinball");
    make_prefix(&paint_prefix, "paint");
    wrong_prefix = pinball_prefix;
    strcpy(wrong_prefix.id, "other");
    pw_launcher_model_init(&model);

    assert(pw_launcher_model_view(&model, &view) == PW_OK);
    assert(view.status == PW_LAUNCHER_STATUS_EMPTY);
    assert(view.selected_index == PW_LAUNCHER_NO_SELECTION);
    assert(view.selected_profile == NULL && view.active_profile == NULL);
    assert(view.last_result == NULL);
    assert(pw_launcher_model_request_launch(&model) == PW_ERR_STATE);
    assert(pw_launcher_model_select(&model, 0u) == PW_ERR_NOT_FOUND);
    assert(pw_launcher_model_profile_at(&model, 0u, &listed) == PW_ERR_NOT_FOUND);
    assert(listed == NULL);

    assert(pw_launcher_model_add(&model, &pinball, &pinball_prefix) == PW_OK);
    assert(pw_launcher_model_view(&model, &view) == PW_OK);
    assert(view.status == PW_LAUNCHER_STATUS_READY);
    assert(view.selected_index == 0u);
    assert(strcmp(view.selected_profile->id, "space-cadet-pinball") == 0);
    assert(pw_launcher_model_profile_at(&model, 0u, &listed) == PW_OK);
    assert(strcmp(listed->name, "Space Cadet Pinball") == 0);

    assert(pw_launcher_model_add(&model, &paint, &paint_prefix) == PW_OK);
    assert(model.count == 2u);
    assert(pw_launcher_model_profile_at(&model, 1u, &listed) == PW_OK);
    assert(strcmp(listed->id, "paint") == 0);
    listed = &pinball;
    assert(pw_launcher_model_profile_at(&model, 2u, &listed) == PW_ERR_NOT_FOUND);
    assert(listed == &pinball);
    assert(pw_launcher_model_select(&model, 1u) == PW_OK);
    assert(pw_launcher_model_view(&model, &view) == PW_OK);
    assert(view.selected_index == 1u);
    assert(strcmp(view.selected_profile->id, "paint") == 0);

    assert(pw_launcher_model_add(&model, &paint, &paint_prefix) == PW_ERR_MALFORMED);
    wrong_profile = paint;
    strcpy(wrong_profile.id, "paint-other");
    assert(pw_launcher_model_add(&model, &wrong_profile, &pinball_prefix) ==
           PW_ERR_MALFORMED);
    assert(pw_launcher_model_add(&model, &pinball, &wrong_prefix) == PW_ERR_MALFORMED);
    assert(model.count == 2u);
}

static void test_launch_stop_cleanup_lifecycle(void)
{
    PwLauncherModel model;
    PwLauncherView view;
    PwAppProfile pinball, paint;
    PwPrefixLayout pinball_prefix, paint_prefix;

    parse_profiles(&pinball, &paint);
    make_prefix(&pinball_prefix, "pinball");
    make_prefix(&paint_prefix, "paint");
    pw_launcher_model_init(&model);
    assert(pw_launcher_model_add(&model, &pinball, &pinball_prefix) == PW_OK);
    assert(pw_launcher_model_add(&model, &paint, &paint_prefix) == PW_OK);
    assert(pw_launcher_model_select(&model, 1u) == PW_OK);

    assert(pw_launcher_model_guest_started(&model) == PW_ERR_STATE);
    assert(pw_launcher_model_request_stop(&model) == PW_ERR_STATE);
    assert(pw_launcher_model_guest_exited(&model, 0) == PW_ERR_STATE);
    assert(pw_launcher_model_cleanup_complete(&model, PW_OK) == PW_ERR_STATE);
    assert(pw_launcher_model_fail(&model, PW_ERR_VM) == PW_ERR_STATE);

    assert(pw_launcher_model_request_launch(&model) == PW_OK);
    assert(pw_launcher_model_request_launch(&model) == PW_ERR_STATE);
    assert(pw_launcher_model_view(&model, &view) == PW_OK);
    assert(view.status == PW_LAUNCHER_STATUS_PREPARING);
    assert(view.selected_profile && view.active_profile);
    assert(strcmp(view.active_profile->id, "paint") == 0);
    assert(view.last_result && view.last_result->session_id == 1u);
    assert(pw_launcher_model_select(&model, 0u) == PW_ERR_STATE);
    assert(pw_launcher_model_add(&model, &pinball, &pinball_prefix) == PW_ERR_STATE);

    assert(pw_launcher_model_guest_started(&model) == PW_OK);
    assert(pw_launcher_model_guest_started(&model) == PW_ERR_STATE);
    assert(pw_launcher_model_view(&model, &view) == PW_OK);
    assert(view.status == PW_LAUNCHER_STATUS_RUNNING);
    assert(pw_launcher_model_request_stop(&model) == PW_OK);
    assert(pw_launcher_model_request_stop(&model) == PW_OK);
    assert(pw_launcher_model_view(&model, &view) == PW_OK);
    assert(view.status == PW_LAUNCHER_STATUS_STOPPING);
    assert(pw_launcher_model_guest_started(&model) == PW_OK);
    assert(pw_launcher_model_view(&model, &view) == PW_OK);
    assert(view.status == PW_LAUNCHER_STATUS_STOPPING);

    assert(pw_launcher_model_guest_exited(&model, 23) == PW_OK);
    assert(pw_launcher_model_guest_exited(&model, 23) == PW_ERR_STATE);
    assert(pw_launcher_model_view(&model, &view) == PW_OK);
    assert(view.status == PW_LAUNCHER_STATUS_CLEANUP);
    assert(view.last_result->outcome == PW_RUNTIME_OUTCOME_EXITED);
    assert(view.last_result->guest_status == 23);
    assert(pw_launcher_model_cleanup_complete(&model, PW_ERR_VM) == PW_ERR_VM);
    assert(pw_launcher_model_view(&model, &view) == PW_OK);
    assert(view.status == PW_LAUNCHER_STATUS_CLEANUP);
    assert(view.active_profile != NULL);
    assert(pw_launcher_model_return_to_catalogue(&model) == PW_ERR_STATE);
    assert(view.last_result->cleanup_status == PW_ERR_VM);
    assert(pw_launcher_model_request_launch(&model) == PW_ERR_STATE);

    assert(pw_launcher_model_cleanup_complete(&model, PW_OK) == PW_OK);
    assert(pw_launcher_model_view(&model, &view) == PW_OK);
    assert(view.status == PW_LAUNCHER_STATUS_COMPLETE);
    assert(view.active_profile == NULL);
    assert(view.last_result->session_id == 1u);
    assert(view.last_result->outcome == PW_RUNTIME_OUTCOME_EXITED);
    assert(view.last_result->cleanup_status == PW_OK);
    assert(pw_launcher_model_request_launch(&model) == PW_ERR_STATE);
    assert(pw_launcher_model_return_to_catalogue(&model) == PW_OK);
    assert(pw_launcher_model_view(&model, &view) == PW_OK);
    assert(view.status == PW_LAUNCHER_STATUS_CATALOGUE);
    assert(view.selected_index == 1u);
    assert(strcmp(view.selected_profile->id, "paint") == 0);
    assert(view.last_result == NULL);
    assert(pw_launcher_model_return_to_catalogue(&model) == PW_ERR_STATE);
}

static void test_failed_launch_and_retry(void)
{
    PwLauncherModel model;
    PwLauncherView view;
    PwAppProfile pinball, paint;
    PwPrefixLayout pinball_prefix;

    parse_profiles(&pinball, &paint);
    make_prefix(&pinball_prefix, "pinball");
    pw_launcher_model_init(&model);
    assert(pw_launcher_model_add(&model, &pinball, &pinball_prefix) == PW_OK);
    assert(pw_launcher_model_request_launch(&model) == PW_OK);
    assert(pw_launcher_model_fail(&model, PW_ERR_UNSUPPORTED) == PW_OK);
    assert(pw_launcher_model_view(&model, &view) == PW_OK);
    assert(view.status == PW_LAUNCHER_STATUS_CLEANUP);
    assert(view.last_result->outcome == PW_RUNTIME_OUTCOME_FAILED);
    assert(view.last_result->guest_status == PW_ERR_UNSUPPORTED);
    assert(pw_launcher_model_cleanup_complete(&model, PW_ERR_VM) == PW_ERR_VM);
    assert(pw_launcher_model_view(&model, &view) == PW_OK);
    assert(view.status == PW_LAUNCHER_STATUS_CLEANUP);
    assert(view.active_profile != NULL);
    assert(view.last_result->outcome == PW_RUNTIME_OUTCOME_FAILED);
    assert(view.last_result->cleanup_status == PW_ERR_VM);
    assert(pw_launcher_model_return_to_catalogue(&model) == PW_ERR_STATE);
    assert(pw_launcher_model_cleanup_complete(&model, PW_OK) == PW_OK);
    assert(pw_launcher_model_view(&model, &view) == PW_OK);
    assert(view.status == PW_LAUNCHER_STATUS_FAILED);
    assert(view.active_profile == NULL);
    assert(pw_launcher_model_select(&model, 1u) == PW_ERR_STATE);
    assert(pw_launcher_model_request_launch(&model) == PW_ERR_STATE);
    assert(pw_launcher_model_return_to_catalogue(&model) == PW_OK);
    assert(pw_launcher_model_view(&model, &view) == PW_OK);
    assert(view.status == PW_LAUNCHER_STATUS_CATALOGUE);

    assert(pw_launcher_model_request_launch(&model) == PW_OK);
    assert(model.supervisor.last_result.session_id == 2u);
    assert(pw_launcher_model_request_stop(&model) == PW_OK);
    assert(pw_launcher_model_guest_exited(&model, 0) == PW_OK);
    assert(pw_launcher_model_cleanup_complete(&model, PW_OK) == PW_OK);
    assert(pw_launcher_model_view(&model, &view) == PW_OK);
    assert(view.status == PW_LAUNCHER_STATUS_COMPLETE);
    assert(view.last_result->session_id == 2u);
}

static void test_profile_capacity(void)
{
    PwLauncherModel model;
    PwAppProfile profile, parsed_paint;
    PwPrefixLayout prefix;
    char id[PW_APP_ID_CAPACITY];

    parse_profiles(&profile, &parsed_paint);
    make_prefix(&prefix, "pinball");
    pw_launcher_model_init(&model);
    for (uint32_t index = 0u; index < PW_LAUNCHER_MAX_PROFILES; ++index) {
        assert(snprintf(id, sizeof(id), "app-%02u", index) > 0);
        profile = (PwAppProfile){0};
        parse_profiles(&profile, &parsed_paint);
        assert(snprintf(profile.id, sizeof(profile.id), "%s", id) > 0);
        assert(pw_launcher_model_add(&model, &profile, &prefix) == PW_OK);
    }
    assert(model.count == PW_LAUNCHER_MAX_PROFILES);
    assert(pw_launcher_model_add(&model, &profile, &prefix) == PW_ERR_LIMIT);
}

int main(void)
{
    test_empty_add_and_enumeration();
    test_launch_stop_cleanup_lifecycle();
    test_failed_launch_and_retry();
    test_profile_capacity();
    return 0;
}
