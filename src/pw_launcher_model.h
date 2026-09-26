/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PROSPERO_WIN_PW_LAUNCHER_MODEL_H
#define PROSPERO_WIN_PW_LAUNCHER_MODEL_H

#include "pw_runtime_supervisor.h"

enum { PW_LAUNCHER_MAX_PROFILES = 16 };
#define PW_LAUNCHER_NO_SELECTION UINT32_MAX

typedef struct PwLauncherEntry {
    PwAppProfile profile;
    PwPrefixLayout prefix;
} PwLauncherEntry;

/* State for a UI/controller. It stores parsed profiles and drives only the
 * existing supervisor contract; it never loads or executes guest code. */
typedef struct PwLauncherModel {
    PwLauncherEntry entries[PW_LAUNCHER_MAX_PROFILES];
    uint32_t count;
    uint32_t selected_index;
    int catalogue_active;
    PwRuntimeSupervisor supervisor;
} PwLauncherModel;

typedef enum PwLauncherStatus {
    PW_LAUNCHER_STATUS_EMPTY = 0,
    PW_LAUNCHER_STATUS_READY,
    PW_LAUNCHER_STATUS_PREPARING,
    PW_LAUNCHER_STATUS_RUNNING,
    PW_LAUNCHER_STATUS_STOPPING,
    PW_LAUNCHER_STATUS_CLEANUP,
    PW_LAUNCHER_STATUS_COMPLETE,
    PW_LAUNCHER_STATUS_FAILED,
    PW_LAUNCHER_STATUS_CATALOGUE,
} PwLauncherStatus;

typedef struct PwLauncherView {
    PwLauncherStatus status;
    uint32_t selected_index;
    const PwAppProfile *selected_profile;
    const PwAppProfile *active_profile;
    const PwRuntimeResult *last_result;
} PwLauncherView;

void pw_launcher_model_init(PwLauncherModel *model);
int pw_launcher_model_add(PwLauncherModel *model,
                          const PwAppProfile *profile,
                          const PwPrefixLayout *prefix);
int pw_launcher_model_profile_at(const PwLauncherModel *model, uint32_t index,
                                 const PwAppProfile **profile);
int pw_launcher_model_select(PwLauncherModel *model, uint32_t index);

/* A launch request enters PREPARING; the caller performs platform work and
 * reports lifecycle events through the following explicit transitions. */
int pw_launcher_model_request_launch(PwLauncherModel *model);
int pw_launcher_model_guest_started(PwLauncherModel *model);
int pw_launcher_model_request_stop(PwLauncherModel *model);
int pw_launcher_model_guest_exited(PwLauncherModel *model,
                                   int32_t guest_status);
int pw_launcher_model_fail(PwLauncherModel *model, int failure_status);
int pw_launcher_model_cleanup_complete(PwLauncherModel *model,
                                       int cleanup_status);
/* Acknowledge the completed/failed session and return to catalogue state.
 * This requires successful cleanup, preserves entries/selection/session ids,
 * and clears only the displayed last-result snapshot. */
int pw_launcher_model_return_to_catalogue(PwLauncherModel *model);
int pw_launcher_model_view(const PwLauncherModel *model, PwLauncherView *view);

#endif
