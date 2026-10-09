/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_BATCH_PIPELINE_H
#define PW_D3D9_BATCH_PIPELINE_H
#include "pw_d3d9_command_batch.h"
#include "pw_d3d9_bridge_wire.h"

#define PW_D3D9_PIPELINE_CREDITS 8u
enum pw_d3d9_pipeline_result {
    PW_D3D9_PIPELINE_OK, PW_D3D9_PIPELINE_FULL, PW_D3D9_PIPELINE_INVALID,
    PW_D3D9_PIPELINE_FAILED, PW_D3D9_PIPELINE_EMPTY
};
/* Local metadata only. slot identifies caller-owned resource ticket storage.
 * No pointers, COM references, clocks or allocations are managed here. */
struct pw_d3d9_pipeline_entry {
    uint64_t ticket, first_sequence;
    uint32_t object, generation, count, frame_bytes, slot;
};
struct pw_d3d9_batch_pipeline {
    struct pw_d3d9_pipeline_entry entries[PW_D3D9_PIPELINE_CREDITS];
    uint64_t next_command, last_ticket;
    uint32_t epoch, head, count, reserved, failure;
};
int pw_d3d9_pipeline_init(struct pw_d3d9_batch_pipeline *, uint32_t epoch,
                          uint64_t first_command);
/* Serialized reserve/send/commit. FULL must NOT publish. Abort is valid only
 * if transport did not publish; a published send followed by wake failure
 * must commit then cancel. Neither reserve nor abort advances sequences. */
int pw_d3d9_pipeline_reserve(struct pw_d3d9_batch_pipeline *,
                             const struct pw_d3d9_pipeline_entry *, uint32_t *slot);
int pw_d3d9_pipeline_commit(struct pw_d3d9_batch_pipeline *);
int pw_d3d9_pipeline_abort(struct pw_d3d9_batch_pipeline *);
/* Caller has already copied the wire reply. All identity, sequence and typed
 * prefix checks precede retirement. FAILED with retired output changed means
 * a valid native failure ACK: its tickets may retire, later entries may not.
 * INVALID poisons the ledger and leaves both outputs unchanged. */
int pw_d3d9_pipeline_ack(struct pw_d3d9_batch_pipeline *, uint32_t epoch,
                         const struct pw_d3d9_message *, const void *, size_t,
                         struct pw_d3d9_pipeline_entry *retired,
                         struct pw_d3d9_batch_reply *reply);
void pw_d3d9_pipeline_cancel(struct pw_d3d9_batch_pipeline *, uint32_t hresult);
/* Unacknowledged entries can be returned for cleanup ONLY after the service
 * broker has joined. This assertion is supplied by the lifecycle owner.
 * Returned ticket drops still belong outside the admission lock. */
int pw_d3d9_pipeline_abandon(struct pw_d3d9_batch_pipeline *, int service_joined,
                             struct pw_d3d9_pipeline_entry *retired);
#endif
