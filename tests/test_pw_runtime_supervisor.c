/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_runtime_supervisor.h"

#include <assert.h>
#include <string.h>

static const uint8_t profile_data[] =
    "[application]\n"
    "id=space-cadet-pinball\n"
    "name=3D Pinball\n"
    "executable=C:\\Games\\Pinball\\PINBALL.EXE\n"
    "working_directory=C:\\Games\\Pinball\n"
    "prefix=pinball\n"
    "runtime=wine-i386-pinned\n"
    "architecture=pe32\n"
    "graphics=gdi\n";

static void make_profile(PwAppProfile *profile)
{
    assert(pw_app_profile_parse(profile_data, sizeof(profile_data) - 1u,
                                profile) == PW_OK);
}

static void make_prefix(PwPrefixLayout *prefix)
{
    memset(prefix, 0, sizeof(*prefix));
    strcpy(prefix->id, "pinball");
    strcpy(prefix->root, "/data/prospero-win/prefixes/pinball");
    strcpy(prefix->drive_c, "/data/prospero-win/prefixes/pinball/drive_c");
    strcpy(prefix->hives[PW_PREFIX_HIVE_SYSTEM],
           "/data/prospero-win/prefixes/pinball/system.reg");
    strcpy(prefix->hives[PW_PREFIX_HIVE_USER],
           "/data/prospero-win/prefixes/pinball/user.reg");
}

int main(void)
{
    PwRuntimeSupervisor supervisor;
    PwAppProfile profile;
    PwPrefixLayout prefix;

    make_profile(&profile);
    make_prefix(&prefix);
    pw_runtime_supervisor_init(&supervisor);
    assert(supervisor.state == PW_RUNTIME_IDLE);
    assert(pw_runtime_supervisor_guest_started(&supervisor) == PW_ERR_STATE);
    assert(pw_runtime_supervisor_request_stop(&supervisor) == PW_ERR_STATE);
    assert(pw_runtime_supervisor_guest_exited(&supervisor, 0) == PW_ERR_STATE);
    assert(pw_runtime_supervisor_fail(&supervisor, PW_ERR_VM) == PW_ERR_STATE);
    assert(pw_runtime_supervisor_cleanup_complete(&supervisor, PW_OK) == PW_ERR_STATE);

    {
        PwPrefixLayout wrong_prefix = prefix;
        strcpy(wrong_prefix.id, "paint");
        assert(pw_runtime_supervisor_begin(&supervisor, &profile, &wrong_prefix) ==
               PW_ERR_MALFORMED);
        assert(supervisor.state == PW_RUNTIME_IDLE);
        assert(supervisor.next_session_id == 0u);
    }

    assert(pw_runtime_supervisor_begin(&supervisor, &profile, &prefix) == PW_OK);
    assert(supervisor.state == PW_RUNTIME_PREPARING);
    assert(supervisor.last_result.session_id == 1u);
    assert(strcmp(supervisor.active_profile.id, profile.id) == 0);
    assert(strcmp(supervisor.active_prefix.root, prefix.root) == 0);
    assert(pw_runtime_supervisor_begin(&supervisor, &profile, &prefix) == PW_ERR_STATE);
    assert(pw_runtime_supervisor_guest_started(&supervisor) == PW_OK);
    assert(supervisor.state == PW_RUNTIME_RUNNING);
    assert(pw_runtime_supervisor_request_stop(&supervisor) == PW_OK);
    assert(supervisor.state == PW_RUNTIME_STOPPING);
    assert(pw_runtime_supervisor_request_stop(&supervisor) == PW_OK);
    assert(pw_runtime_supervisor_guest_exited(&supervisor, 0) == PW_OK);
    assert(supervisor.state == PW_RUNTIME_CLEANUP);
    assert(supervisor.last_result.outcome == PW_RUNTIME_OUTCOME_EXITED);
    assert(supervisor.last_result.guest_status == 0);
    assert(pw_runtime_supervisor_cleanup_complete(&supervisor, PW_ERR_VM) == PW_ERR_VM);
    assert(supervisor.state == PW_RUNTIME_CLEANUP);
    assert(supervisor.active_profile.id[0] != '\0');
    assert(supervisor.last_result.cleanup_status == PW_ERR_VM);
    assert(pw_runtime_supervisor_begin(&supervisor, &profile, &prefix) == PW_ERR_STATE);
    assert(pw_runtime_supervisor_cleanup_complete(&supervisor, PW_OK) == PW_OK);
    assert(supervisor.state == PW_RUNTIME_IDLE);
    assert(supervisor.active_profile.id[0] == '\0');
    assert(supervisor.last_result.session_id == 1u);
    assert(supervisor.last_result.outcome == PW_RUNTIME_OUTCOME_EXITED);
    assert(supervisor.last_result.cleanup_status == PW_OK);

    assert(pw_runtime_supervisor_begin(&supervisor, &profile, &prefix) == PW_OK);
    assert(supervisor.last_result.session_id == 2u);
    assert(pw_runtime_supervisor_fail(&supervisor, PW_ERR_UNSUPPORTED) == PW_OK);
    assert(supervisor.state == PW_RUNTIME_CLEANUP);
    assert(supervisor.last_result.outcome == PW_RUNTIME_OUTCOME_FAILED);
    assert(supervisor.last_result.guest_status == PW_ERR_UNSUPPORTED);
    assert(pw_runtime_supervisor_cleanup_complete(&supervisor, PW_OK) == PW_OK);

    assert(pw_runtime_supervisor_begin(&supervisor, &profile, &prefix) == PW_OK);
    assert(pw_runtime_supervisor_request_stop(&supervisor) == PW_OK);
    assert(supervisor.state == PW_RUNTIME_STOPPING);
    /* A guest that finishes startup after Stop was requested remains stopping. */
    assert(pw_runtime_supervisor_guest_started(&supervisor) == PW_OK);
    assert(supervisor.state == PW_RUNTIME_STOPPING);
    assert(pw_runtime_supervisor_guest_exited(&supervisor, 23) == PW_OK);
    assert(pw_runtime_supervisor_cleanup_complete(&supervisor, PW_OK) == PW_OK);
    assert(supervisor.last_result.guest_status == 23);

    {
        PwAppProfile invalid = profile;
        memset(invalid.arguments, 'x', sizeof(invalid.arguments));
        assert(pw_runtime_supervisor_begin(&supervisor, &invalid, &prefix) ==
               PW_ERR_MALFORMED);
        assert(supervisor.state == PW_RUNTIME_IDLE);
        supervisor.next_session_id = UINT32_MAX;
        assert(pw_runtime_supervisor_begin(&supervisor, &profile, &prefix) ==
               PW_ERR_LIMIT);
        assert(supervisor.state == PW_RUNTIME_IDLE);
    }
    assert(pw_runtime_supervisor_begin(NULL, &profile, &prefix) == PW_ERR_PRECONDITION);
    assert(pw_runtime_supervisor_begin(&supervisor, NULL, &prefix) == PW_ERR_PRECONDITION);
    assert(pw_runtime_supervisor_begin(&supervisor, &profile, NULL) == PW_ERR_PRECONDITION);
    pw_runtime_supervisor_init(NULL);
    return 0;
}
