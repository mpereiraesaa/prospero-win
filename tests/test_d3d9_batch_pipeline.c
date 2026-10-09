/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_batch_pipeline.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct pw_d3d9_pipeline_entry entry(uint64_t ticket, uint64_t first)
{
    return (struct pw_d3d9_pipeline_entry){ticket, first, 2, 9, 3, 256, 0};
}
static void publish(struct pw_d3d9_batch_pipeline *p, uint64_t ticket)
{
    struct pw_d3d9_pipeline_entry e = entry(ticket, p->next_command); uint32_t slot = 99;
    assert(pw_d3d9_pipeline_reserve(p, &e, &slot) == PW_D3D9_PIPELINE_OK);
    assert(slot < PW_D3D9_PIPELINE_CREDITS);
    assert(pw_d3d9_pipeline_commit(p) == PW_D3D9_PIPELINE_OK);
}
static int ack(struct pw_d3d9_batch_pipeline *p, unsigned fault, uint32_t hr,
               struct pw_d3d9_pipeline_entry *retired)
{
    struct pw_d3d9_pipeline_entry e = p->entries[p->head];
    struct pw_d3d9_batch_reply q = {e.first_sequence, e.count, hr ? 2 : e.count, hr ? 1 : UINT32_MAX, hr};
    unsigned char bytes[PW_D3D9_BATCH_REPLY];
    assert(pw_d3d9_batch_reply_encode(bytes, sizeof(bytes), &q) == PW_D3D9_BATCH_OK);
    struct pw_d3d9_message m = {PW_D3D9_COMMAND_BATCH_CALL, 1, e.object, e.generation,
                               e.ticket, e.ticket, (int32_t)hr, sizeof(bytes)};
    uint32_t epoch = p->epoch;
    switch (fault) {
    case 1: m.ticket++; break;
    case 2: m.sequence++; break;
    case 3: m.object++; break;
    case 4: m.generation++; break;
    case 5: epoch++; break;
    case 6: m.opcode = PW_D3D9_COMMAND_CALL; break;
    case 7: m.device = 0; break;
    case 8: m.payload_bytes--; break;
    case 9: m.result = (int32_t)0x80070057u; break;
    case 10: bytes[8]++; break; /* command sequence */
    case 11: bytes[4]++; break; /* count */
    case 12: bytes[28] = 1; break; /* reserved */
    case 13: bytes[16] = 0; break; /* attempted prefix */
    case 14: bytes[24] = 1; break; /* unexpected positive HRESULT */
    default: break;
    }
    struct pw_d3d9_batch_reply result, before;
    memset(&result, 0x5a, sizeof(result)); before = result;
    int status = pw_d3d9_pipeline_ack(p, epoch, &m, bytes, sizeof(bytes), retired, &result);
    if (fault) assert(!memcmp(&result, &before, sizeof(result)));
    else assert(result.hresult == hr && result.attempted == q.attempted);
    return status;
}
int main(void)
{
    struct pw_d3d9_batch_pipeline p, before;
    struct pw_d3d9_pipeline_entry retired, saved;
    uint32_t slot = 99;
    memset(&p, 0x5a, sizeof(p)); before = p;
    assert(pw_d3d9_pipeline_init(&p, 0, 1) == PW_D3D9_PIPELINE_INVALID);
    assert(!memcmp(&p, &before, sizeof(p)));
    assert(pw_d3d9_pipeline_init(&p, 7, 1) == PW_D3D9_PIPELINE_OK);
    struct pw_d3d9_pipeline_entry e = entry(2, 1);
    assert(pw_d3d9_pipeline_reserve(&p, &e, &slot) == PW_D3D9_PIPELINE_OK);
    assert(p.count == 0 && p.next_command == 1);
    assert(pw_d3d9_pipeline_abort(&p) == PW_D3D9_PIPELINE_OK);
    assert(p.next_command == 1 && p.last_ticket == 0);
    /* Eight credits, FIFO ACKs and repeated ring wrap. Gaps in outer tickets
     * model synchronous boundaries; inner command ranges remain contiguous. */
    uint64_t ticket = 2;
    for (unsigned round = 0; round < 2000; round++) {
        for (unsigned i = 0; i < PW_D3D9_PIPELINE_CREDITS; i++) publish(&p, ticket++);
        before = p; e = entry(ticket, p.next_command); slot = 99;
        assert(pw_d3d9_pipeline_reserve(&p, &e, &slot) == PW_D3D9_PIPELINE_FULL);
        assert(slot == 99 && !memcmp(&p, &before, sizeof(p)));
        for (unsigned i = 0; i < PW_D3D9_PIPELINE_CREDITS; i++) {
            uint64_t expected = p.entries[p.head].ticket;
            assert(ack(&p, 0, 0, &retired) == PW_D3D9_PIPELINE_OK);
            assert(retired.ticket == expected);
        }
        ticket += 2;
    }
    assert(p.count == 0 && p.next_command == 48001);
    /* Every malformed ACK quarantines ALL entries and publishes no outputs. */
    for (unsigned fault = 1; fault <= 14; fault++) {
        assert(!pw_d3d9_pipeline_init(&p, 7, 1)); publish(&p, 2); publish(&p, 3);
        memset(&retired, 0xa5, sizeof(retired)); saved = retired;
        assert(ack(&p, fault, 0, &retired) == PW_D3D9_PIPELINE_INVALID);
        assert(p.count == 2 && p.failure == 0x80004005u && !memcmp(&retired, &saved, sizeof(saved)));
        assert(pw_d3d9_pipeline_abandon(&p, 0, &retired) == PW_D3D9_PIPELINE_INVALID);
        assert(!memcmp(&retired, &saved, sizeof(saved)));
        assert(!pw_d3d9_pipeline_abandon(&p, 1, &retired) && retired.ticket == 2);
        assert(!pw_d3d9_pipeline_abandon(&p, 1, &retired) && retired.ticket == 3);
        assert(pw_d3d9_pipeline_abandon(&p, 1, &retired) == PW_D3D9_PIPELINE_EMPTY);
    }
    /* Native failure retires only its acknowledged batch. Later published
     * resource tickets survive until broker join; first HRESULT is sticky. */
    assert(!pw_d3d9_pipeline_init(&p, 7, 1)); publish(&p, 2); publish(&p, 3); publish(&p, 4);
    assert(ack(&p, 0, 0x8876086cu, &retired) == PW_D3D9_PIPELINE_FAILED);
    assert(retired.ticket == 2 && p.count == 2 && p.failure == 0x8876086cu);
    pw_d3d9_pipeline_cancel(&p, 0x80004005u); assert(p.failure == 0x8876086cu);
    e = entry(5, p.next_command); assert(pw_d3d9_pipeline_reserve(&p, &e, &slot) == PW_D3D9_PIPELINE_FAILED);
    assert(pw_d3d9_pipeline_abandon(&p, 0, &retired) == PW_D3D9_PIPELINE_INVALID);
    assert(!pw_d3d9_pipeline_abandon(&p, 1, &retired) && retired.ticket == 3);
    assert(!pw_d3d9_pipeline_abandon(&p, 1, &retired) && retired.ticket == 4);
    /* Cancellation after publication but before commit must retain ownership. */
    assert(!pw_d3d9_pipeline_init(&p, 7, 1)); e = entry(2, 1);
    assert(!pw_d3d9_pipeline_reserve(&p, &e, &slot)); pw_d3d9_pipeline_cancel(&p, 0);
    assert(!pw_d3d9_pipeline_commit(&p)); assert(p.count == 1);
    assert(!pw_d3d9_pipeline_abandon(&p, 1, &retired));
    assert(!pw_d3d9_pipeline_init(&p, 7, UINT64_MAX)); e = entry(2, UINT64_MAX);
    assert(pw_d3d9_pipeline_reserve(&p, &e, &slot) == PW_D3D9_PIPELINE_INVALID);
    assert(!pw_d3d9_pipeline_init(&p, 7, 1)); e = entry(UINT64_MAX, 1);
    assert(pw_d3d9_pipeline_reserve(&p, &e, &slot) == PW_D3D9_PIPELINE_INVALID);
    puts("D3D9 pipeline ledger PASS"); return 0;
}
