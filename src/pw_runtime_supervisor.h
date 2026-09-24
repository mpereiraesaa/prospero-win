/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PROSPERO_WIN_PW_RUNTIME_SUPERVISOR_H
#define PROSPERO_WIN_PW_RUNTIME_SUPERVISOR_H

#include "pw_app_profile.h"
#include "pw_prefix.h"

typedef enum PwRuntimeState {
    PW_RUNTIME_IDLE = 0,
    PW_RUNTIME_PREPARING = 1,
    PW_RUNTIME_RUNNING = 2,
    PW_RUNTIME_STOPPING = 3,
    PW_RUNTIME_CLEANUP = 4,
} PwRuntimeState;

typedef enum PwRuntimeOutcome {
    PW_RUNTIME_OUTCOME_NONE = 0,
    PW_RUNTIME_OUTCOME_EXITED = 1,
    PW_RUNTIME_OUTCOME_FAILED = 2,
} PwRuntimeOutcome;

typedef struct PwRuntimeResult {
    uint32_t session_id;
    PwRuntimeOutcome outcome;
    int32_t guest_status;
    int cleanup_status;
} PwRuntimeResult;

/* Single-owner, single-active-guest lifecycle coordinator. The caller drives
 * each event after the platform backend completes its corresponding action. */
typedef struct PwRuntimeSupervisor {
    PwRuntimeState state;
    uint32_t next_session_id;
    PwRuntimeResult last_result;
    PwAppProfile active_profile;
    PwPrefixLayout active_prefix;
} PwRuntimeSupervisor;

void pw_runtime_supervisor_init(PwRuntimeSupervisor *supervisor);

/* Binds a parsed profile to its opened prefix and starts a new session in
 * PREPARING. Only one guest may be active; the profile prefix id must match. */
int pw_runtime_supervisor_begin(PwRuntimeSupervisor *supervisor,
                                const PwAppProfile *profile,
                                const PwPrefixLayout *prefix);

int pw_runtime_supervisor_guest_started(PwRuntimeSupervisor *supervisor);
int pw_runtime_supervisor_request_stop(PwRuntimeSupervisor *supervisor);
int pw_runtime_supervisor_guest_exited(PwRuntimeSupervisor *supervisor,
                                       int32_t guest_status);
int pw_runtime_supervisor_fail(PwRuntimeSupervisor *supervisor,
                               int failure_status);

/* A failed cleanup leaves the session and ownership data intact in CLEANUP so
 * the platform adapter can retry. Successful cleanup returns to IDLE while
 * retaining the final result for the UI and telemetry layer. */
int pw_runtime_supervisor_cleanup_complete(PwRuntimeSupervisor *supervisor,
                                           int cleanup_status);

#endif
