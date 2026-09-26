/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_launcher_model.h"

#include <string.h>

static int terminated_nonempty(const char *value, size_t capacity)
{
    const char *end;
    if (!value || !value[0])
        return 0;
    end = memchr(value, '\0', capacity);
    return end != NULL && end != value;
}

void pw_launcher_model_init(PwLauncherModel *model)
{
    if (!model)
        return;
    memset(model, 0, sizeof(*model));
    model->selected_index = PW_LAUNCHER_NO_SELECTION;
    pw_runtime_supervisor_init(&model->supervisor);
}

int pw_launcher_model_add(PwLauncherModel *model,
                          const PwAppProfile *profile,
                          const PwPrefixLayout *prefix)
{
    if (!model || !profile || !prefix)
        return PW_ERR_PRECONDITION;
    if (model->supervisor.state != PW_RUNTIME_IDLE)
        return PW_ERR_STATE;
    if (model->supervisor.last_result.outcome != PW_RUNTIME_OUTCOME_NONE)
        return PW_ERR_STATE;
    if (!terminated_nonempty(profile->id, sizeof(profile->id)) ||
        !terminated_nonempty(profile->prefix, sizeof(profile->prefix)) ||
        !terminated_nonempty(prefix->id, sizeof(prefix->id)))
        return PW_ERR_MALFORMED;
    if (model->count >= PW_LAUNCHER_MAX_PROFILES)
        return PW_ERR_LIMIT;
    for (uint32_t index = 0u; index < model->count; ++index) {
        if (strcmp(model->entries[index].profile.id, profile->id) == 0)
            return PW_ERR_MALFORMED;
    }
    if (strcmp(profile->prefix, prefix->id) != 0)
        return PW_ERR_MALFORMED;
    model->entries[model->count].profile = *profile;
    model->entries[model->count].prefix = *prefix;
    model->count++;
    if (model->selected_index == PW_LAUNCHER_NO_SELECTION)
        model->selected_index = 0u;
    return PW_OK;
}

int pw_launcher_model_select(PwLauncherModel *model, uint32_t index)
{
    if (!model)
        return PW_ERR_PRECONDITION;
    if (model->supervisor.state != PW_RUNTIME_IDLE)
        return PW_ERR_STATE;
    if (model->supervisor.last_result.outcome != PW_RUNTIME_OUTCOME_NONE)
        return PW_ERR_STATE;
    if (index >= model->count)
        return PW_ERR_NOT_FOUND;
    model->selected_index = index;
    return PW_OK;
}

int pw_launcher_model_profile_at(const PwLauncherModel *model, uint32_t index,
                                 const PwAppProfile **profile)
{
    if (!model || !profile)
        return PW_ERR_PRECONDITION;
    if (index >= model->count)
        return PW_ERR_NOT_FOUND;
    *profile = &model->entries[index].profile;
    return PW_OK;
}

int pw_launcher_model_request_launch(PwLauncherModel *model)
{
    const PwLauncherEntry *entry;
    int result;

    if (!model)
        return PW_ERR_PRECONDITION;
    if (model->selected_index == PW_LAUNCHER_NO_SELECTION ||
        model->selected_index >= model->count)
        return PW_ERR_STATE;
    if (model->supervisor.last_result.outcome != PW_RUNTIME_OUTCOME_NONE)
        return PW_ERR_STATE;
    entry = &model->entries[model->selected_index];
    result = pw_runtime_supervisor_begin(&model->supervisor, &entry->profile,
                                         &entry->prefix);
    if (result == PW_OK)
        model->catalogue_active = 0;
    return result;
}

int pw_launcher_model_guest_started(PwLauncherModel *model)
{
    return model ? pw_runtime_supervisor_guest_started(&model->supervisor) :
                   PW_ERR_PRECONDITION;
}

int pw_launcher_model_request_stop(PwLauncherModel *model)
{
    return model ? pw_runtime_supervisor_request_stop(&model->supervisor) :
                   PW_ERR_PRECONDITION;
}

int pw_launcher_model_guest_exited(PwLauncherModel *model,
                                   int32_t guest_status)
{
    return model ? pw_runtime_supervisor_guest_exited(&model->supervisor,
                                                       guest_status) :
                   PW_ERR_PRECONDITION;
}

int pw_launcher_model_fail(PwLauncherModel *model, int failure_status)
{
    return model ? pw_runtime_supervisor_fail(&model->supervisor,
                                               failure_status) :
                   PW_ERR_PRECONDITION;
}

int pw_launcher_model_cleanup_complete(PwLauncherModel *model,
                                       int cleanup_status)
{
    return model ? pw_runtime_supervisor_cleanup_complete(&model->supervisor,
                                                           cleanup_status) :
                   PW_ERR_PRECONDITION;
}

int pw_launcher_model_return_to_catalogue(PwLauncherModel *model)
{
    if (!model)
        return PW_ERR_PRECONDITION;
    if (model->supervisor.state != PW_RUNTIME_IDLE ||
        model->supervisor.last_result.outcome == PW_RUNTIME_OUTCOME_NONE)
        return PW_ERR_STATE;
    memset(&model->supervisor.last_result, 0,
           sizeof(model->supervisor.last_result));
    model->catalogue_active = 1;
    return PW_OK;
}

int pw_launcher_model_view(const PwLauncherModel *model, PwLauncherView *view)
{
    if (!model || !view)
        return PW_ERR_PRECONDITION;
    memset(view, 0, sizeof(*view));
    view->selected_index = model->selected_index;
    view->selected_profile = model->selected_index < model->count ?
        &model->entries[model->selected_index].profile : NULL;
    view->active_profile = model->supervisor.state != PW_RUNTIME_IDLE ?
        &model->supervisor.active_profile : NULL;
    view->last_result = model->supervisor.last_result.session_id != 0u ?
        &model->supervisor.last_result : NULL;

    switch (model->supervisor.state) {
    case PW_RUNTIME_PREPARING: view->status = PW_LAUNCHER_STATUS_PREPARING; break;
    case PW_RUNTIME_RUNNING: view->status = PW_LAUNCHER_STATUS_RUNNING; break;
    case PW_RUNTIME_STOPPING: view->status = PW_LAUNCHER_STATUS_STOPPING; break;
    case PW_RUNTIME_CLEANUP: view->status = PW_LAUNCHER_STATUS_CLEANUP; break;
    case PW_RUNTIME_IDLE:
        if (model->supervisor.last_result.outcome == PW_RUNTIME_OUTCOME_FAILED)
            view->status = PW_LAUNCHER_STATUS_FAILED;
        else if (model->supervisor.last_result.outcome == PW_RUNTIME_OUTCOME_EXITED)
            view->status = PW_LAUNCHER_STATUS_COMPLETE;
        else if (model->catalogue_active)
            view->status = PW_LAUNCHER_STATUS_CATALOGUE;
        else
            view->status = model->count ? PW_LAUNCHER_STATUS_READY :
                                          PW_LAUNCHER_STATUS_EMPTY;
        break;
    default:
        return PW_ERR_STATE;
    }
    return PW_OK;
}
