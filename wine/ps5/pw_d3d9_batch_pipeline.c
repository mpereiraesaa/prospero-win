/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_batch_pipeline.h"
#include <string.h>
#define FAILURE UINT32_C(0x80004005)
_Static_assert(PW_D3D9_PIPELINE_CREDITS < PW_D3D9_WIRE_PENDING, "sync credit reserve");
int pw_d3d9_pipeline_init(struct pw_d3d9_batch_pipeline *p, uint32_t epoch, uint64_t first)
{
    if (!p || !epoch || !first) return PW_D3D9_PIPELINE_INVALID;
    memset(p, 0, sizeof(*p)); p->epoch = epoch; p->next_command = first;
    return PW_D3D9_PIPELINE_OK;
}
void pw_d3d9_pipeline_cancel(struct pw_d3d9_batch_pipeline *p, uint32_t hr)
{
    if (p && !p->failure) p->failure = hr & UINT32_C(0x80000000) ? hr : FAILURE;
}
int pw_d3d9_pipeline_reserve(struct pw_d3d9_batch_pipeline *p,
                             const struct pw_d3d9_pipeline_entry *e, uint32_t *slot)
{
    if (!p || !e || !slot || !p->epoch || p->reserved) return PW_D3D9_PIPELINE_INVALID;
    if (p->failure) return PW_D3D9_PIPELINE_FAILED;
    if (!e->ticket || e->ticket == UINT64_MAX || e->ticket <= p->last_ticket ||
        !e->object || !e->generation || !e->count || e->count > PW_D3D9_BATCH_LIMIT ||
        e->first_sequence != p->next_command || e->count > UINT64_MAX - e->first_sequence ||
        e->frame_bytes < PW_D3D9_WIRE_HEADER + PW_D3D9_BATCH_HEADER ||
        e->frame_bytes > PW_D3D9_WIRE_HEADER + PW_D3D9_BATCH_MAX || (e->frame_bytes & 7))
        return PW_D3D9_PIPELINE_INVALID;
    if (p->count == PW_D3D9_PIPELINE_CREDITS) return PW_D3D9_PIPELINE_FULL;
    uint32_t index = (p->head + p->count) % PW_D3D9_PIPELINE_CREDITS;
    p->entries[index] = *e; p->entries[index].slot = index;
    p->reserved = 1; *slot = index; return PW_D3D9_PIPELINE_OK;
}
int pw_d3d9_pipeline_commit(struct pw_d3d9_batch_pipeline *p)
{
    if (!p || !p->reserved) return PW_D3D9_PIPELINE_INVALID;
    /* Commit remains legal after cancellation racing a successfully published
     * transport send: those tickets must enter the joined-cleanup ledger. */
    const struct pw_d3d9_pipeline_entry *e = &p->entries[(p->head + p->count) % PW_D3D9_PIPELINE_CREDITS];
    p->last_ticket = e->ticket; p->next_command += e->count;
    p->reserved = 0; p->count++; return PW_D3D9_PIPELINE_OK;
}
int pw_d3d9_pipeline_abort(struct pw_d3d9_batch_pipeline *p)
{
    if (!p || !p->reserved) return PW_D3D9_PIPELINE_INVALID;
    p->reserved = 0; return PW_D3D9_PIPELINE_OK;
}
static void pop(struct pw_d3d9_batch_pipeline *p, struct pw_d3d9_pipeline_entry *out)
{
    *out = p->entries[p->head]; p->head = (p->head + 1) % PW_D3D9_PIPELINE_CREDITS; p->count--;
}
int pw_d3d9_pipeline_ack(struct pw_d3d9_batch_pipeline *p, uint32_t epoch,
                         const struct pw_d3d9_message *m, const void *data, size_t bytes,
                         struct pw_d3d9_pipeline_entry *retired, struct pw_d3d9_batch_reply *reply)
{
    if (!p || !retired || !reply) return PW_D3D9_PIPELINE_INVALID;
    if (p->failure) return PW_D3D9_PIPELINE_FAILED;
    if (!p->count || p->reserved || !m || epoch != p->epoch) goto invalid;
    const struct pw_d3d9_pipeline_entry *e = &p->entries[p->head];
    struct pw_d3d9_batch_reply decoded;
    if (m->opcode != PW_D3D9_COMMAND_BATCH_CALL || m->device != 1 ||
        m->object != e->object || m->generation != e->generation ||
        m->sequence != e->ticket || m->ticket != e->ticket || m->payload_bytes != bytes ||
        pw_d3d9_batch_reply_decode(&decoded, data, bytes, e->first_sequence, e->count) != PW_D3D9_BATCH_OK ||
        decoded.hresult != (uint32_t)m->result) goto invalid;
    *reply = decoded; pop(p, retired);
    if (decoded.hresult) { pw_d3d9_pipeline_cancel(p, decoded.hresult); return PW_D3D9_PIPELINE_FAILED; }
    return PW_D3D9_PIPELINE_OK;
invalid:
    pw_d3d9_pipeline_cancel(p, FAILURE); return PW_D3D9_PIPELINE_INVALID;
}
int pw_d3d9_pipeline_abandon(struct pw_d3d9_batch_pipeline *p, int joined,
                             struct pw_d3d9_pipeline_entry *retired)
{
    if (!p || !retired || !p->failure || !joined || p->reserved) return PW_D3D9_PIPELINE_INVALID;
    if (!p->count) return PW_D3D9_PIPELINE_EMPTY;
    pop(p, retired); return PW_D3D9_PIPELINE_OK;
}
