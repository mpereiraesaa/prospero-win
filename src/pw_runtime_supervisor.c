/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_runtime_supervisor.h"

#include <string.h>

static int terminated_nonempty(const char *value, size_t capacity)
{
    const char *end;

    if (!value || !value[0])
        return 0;
    end = memchr(value, '\0', capacity);
    return end != NULL && end != value;
}

static int terminated(const char *value, size_t capacity)
{
    return value && memchr(value, '\0', capacity) != NULL;
}

static int profile_ready(const PwAppProfile *profile)
{
    if (!profile ||
        !terminated_nonempty(profile->id, sizeof(profile->id)) ||
        !terminated_nonempty(profile->name, sizeof(profile->name)) ||
        !terminated_nonempty(profile->executable, sizeof(profile->executable)) ||
        !terminated_nonempty(profile->working_directory,
                             sizeof(profile->working_directory)) ||
        !terminated(profile->arguments, sizeof(profile->arguments)) ||
        !terminated_nonempty(profile->prefix, sizeof(profile->prefix)) ||
        !terminated_nonempty(profile->runtime, sizeof(profile->runtime)))
        return 0;
    if (profile->architecture != PW_APP_ARCH_PE32 &&
        profile->architecture != PW_APP_ARCH_PE64)
        return 0;
    return profile->graphics == PW_APP_GRAPHICS_AUTO ||
           profile->graphics == PW_APP_GRAPHICS_GDI ||
           profile->graphics == PW_APP_GRAPHICS_DXVK;
}

static int prefix_ready(const PwPrefixLayout *prefix)
{
    return prefix &&
        terminated_nonempty(prefix->id, sizeof(prefix->id)) &&
        terminated_nonempty(prefix->root, sizeof(prefix->root)) &&
        prefix->root[0] == '/' &&
        terminated_nonempty(prefix->drive_c, sizeof(prefix->drive_c)) &&
        terminated_nonempty(prefix->hives[PW_PREFIX_HIVE_SYSTEM],
                            sizeof(prefix->hives[PW_PREFIX_HIVE_SYSTEM])) &&
        terminated_nonempty(prefix->hives[PW_PREFIX_HIVE_USER],
                            sizeof(prefix->hives[PW_PREFIX_HIVE_USER]));
}

static int state_can_fail(PwRuntimeState state)
{
    return state == PW_RUNTIME_PREPARING || state == PW_RUNTIME_RUNNING ||
           state == PW_RUNTIME_STOPPING;
}

void pw_runtime_supervisor_init(PwRuntimeSupervisor *supervisor)
{
    if (supervisor)
        memset(supervisor, 0, sizeof(*supervisor));
}

int pw_runtime_supervisor_begin(PwRuntimeSupervisor *supervisor,
                                const PwAppProfile *profile,
                                const PwPrefixLayout *prefix)
{
    if (!supervisor || !profile || !prefix)
        return PW_ERR_PRECONDITION;
    if (supervisor->state != PW_RUNTIME_IDLE)
        return PW_ERR_STATE;
    if (!profile_ready(profile) || !prefix_ready(prefix))
        return PW_ERR_MALFORMED;
    if (strcmp(profile->prefix, prefix->id) != 0)
        return PW_ERR_MALFORMED;
    if (supervisor->next_session_id == UINT32_MAX)
        return PW_ERR_LIMIT;

    supervisor->next_session_id++;
    memset(&supervisor->last_result, 0, sizeof(supervisor->last_result));
    supervisor->last_result.session_id = supervisor->next_session_id;
    supervisor->active_profile = *profile;
    supervisor->active_prefix = *prefix;
    supervisor->state = PW_RUNTIME_PREPARING;
    return PW_OK;
}

int pw_runtime_supervisor_guest_started(PwRuntimeSupervisor *supervisor)
{
    if (!supervisor)
        return PW_ERR_PRECONDITION;
    if (supervisor->state == PW_RUNTIME_STOPPING)
        return PW_OK; /* stop was requested while guest startup was in flight */
    if (supervisor->state != PW_RUNTIME_PREPARING)
        return PW_ERR_STATE;
    supervisor->state = PW_RUNTIME_RUNNING;
    return PW_OK;
}

int pw_runtime_supervisor_request_stop(PwRuntimeSupervisor *supervisor)
{
    if (!supervisor)
        return PW_ERR_PRECONDITION;
    if (supervisor->state == PW_RUNTIME_STOPPING)
        return PW_OK;
    if (supervisor->state != PW_RUNTIME_PREPARING &&
        supervisor->state != PW_RUNTIME_RUNNING)
        return PW_ERR_STATE;
    supervisor->state = PW_RUNTIME_STOPPING;
    return PW_OK;
}

int pw_runtime_supervisor_guest_exited(PwRuntimeSupervisor *supervisor,
                                       int32_t guest_status)
{
    if (!supervisor)
        return PW_ERR_PRECONDITION;
    if (supervisor->state != PW_RUNTIME_RUNNING &&
        supervisor->state != PW_RUNTIME_STOPPING)
        return PW_ERR_STATE;
    supervisor->last_result.outcome = PW_RUNTIME_OUTCOME_EXITED;
    supervisor->last_result.guest_status = guest_status;
    supervisor->state = PW_RUNTIME_CLEANUP;
    return PW_OK;
}

int pw_runtime_supervisor_fail(PwRuntimeSupervisor *supervisor,
                               int failure_status)
{
    if (!supervisor)
        return PW_ERR_PRECONDITION;
    if (!state_can_fail(supervisor->state) || failure_status == PW_OK)
        return PW_ERR_STATE;
    supervisor->last_result.outcome = PW_RUNTIME_OUTCOME_FAILED;
    supervisor->last_result.guest_status = failure_status;
    supervisor->state = PW_RUNTIME_CLEANUP;
    return PW_OK;
}

int pw_runtime_supervisor_cleanup_complete(PwRuntimeSupervisor *supervisor,
                                           int cleanup_status)
{
    if (!supervisor)
        return PW_ERR_PRECONDITION;
    if (supervisor->state != PW_RUNTIME_CLEANUP)
        return PW_ERR_STATE;
    supervisor->last_result.cleanup_status = cleanup_status;
    if (cleanup_status != PW_OK)
        return cleanup_status;
    memset(&supervisor->active_profile, 0, sizeof(supervisor->active_profile));
    memset(&supervisor->active_prefix, 0, sizeof(supervisor->active_prefix));
    supervisor->state = PW_RUNTIME_IDLE;
    return PW_OK;
}
