/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_COMMAND_BATCH_H
#define PW_D3D9_COMMAND_BATCH_H
#include "pw_d3d9_command_wire.h"

#define PW_D3D9_BATCH_VERSION 1u
#define PW_D3D9_BATCH_LIMIT 128u
#define PW_D3D9_BATCH_HEADER 32u
/* Leaves space for framing in the current 8192-byte transport ring. */
#define PW_D3D9_BATCH_MAX 8064u
#define PW_D3D9_BATCH_STORAGE (PW_D3D9_BATCH_MAX - PW_D3D9_BATCH_HEADER)
#define PW_D3D9_BATCH_REPLY 32u
struct pw_d3d9_command_batch {
    uint64_t first_sequence;
    uint32_t count, used, offsets[PW_D3D9_BATCH_LIMIT];
    unsigned char records[PW_D3D9_BATCH_STORAGE];
};
struct pw_d3d9_batch_reply {
    uint64_t first_sequence;
    uint32_t count, attempted, failed_index, hresult;
};
enum pw_d3d9_batch_status {
    PW_D3D9_BATCH_OK, PW_D3D9_BATCH_INVALID, PW_D3D9_BATCH_FULL
};
/* Initialize before append. Append owns command bytes and does not reserve a
 * sequence on failure. Empty batches cannot be encoded. No method is made
 * eligible for asynchronous acceptance by this framing API. */
int pw_d3d9_batch_init(struct pw_d3d9_command_batch *, uint64_t first_sequence);
int pw_d3d9_batch_append(struct pw_d3d9_command_batch *, const struct pw_d3d9_command *);
int pw_d3d9_batch_encode(void *, size_t, size_t *, const struct pw_d3d9_command_batch *);
int pw_d3d9_batch_decode(struct pw_d3d9_command_batch *, const void *, size_t);
/* Use only a successfully decoded or locally built batch. */
int pw_d3d9_batch_command(struct pw_d3d9_command *, const struct pw_d3d9_command_batch *, uint32_t index);
int pw_d3d9_batch_reply_encode(void *, size_t, const struct pw_d3d9_batch_reply *);
/* Correlates every reply against the exact original contiguous command range. */
int pw_d3d9_batch_reply_decode(struct pw_d3d9_batch_reply *, const void *, size_t,
                             uint64_t first_sequence, uint32_t count);
#endif
