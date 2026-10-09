/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_command_batch.h"
#include <string.h>

static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get64(const unsigned char *p)
{ return get32(p) | (uint64_t)get32(p + 4) << 32; }
static void put32(unsigned char *p, uint32_t v)
{ for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i)); }
static void put64(unsigned char *p, uint64_t v)
{ put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static int range_valid(uint64_t first, uint32_t count)
{ return first && count && count <= PW_D3D9_BATCH_LIMIT && first <= UINT64_MAX - (count - 1); }

int pw_d3d9_batch_init(struct pw_d3d9_command_batch *b, uint64_t first)
{
    if (!b || !first) return PW_D3D9_BATCH_INVALID;
    memset(b, 0, sizeof(*b)); b->first_sequence = first;
    return PW_D3D9_BATCH_OK;
}
int pw_d3d9_batch_append(struct pw_d3d9_command_batch *b, const struct pw_d3d9_command *q)
{
    unsigned char command[PW_D3D9_COMMAND_MAX]; size_t bytes;
    if (!b || !q || !b->first_sequence || b->count > PW_D3D9_BATCH_LIMIT ||
        b->used > PW_D3D9_BATCH_STORAGE || (b->used & 7) || (!b->count != !b->used))
        return PW_D3D9_BATCH_INVALID;
    if (b->count == PW_D3D9_BATCH_LIMIT) return PW_D3D9_BATCH_FULL;
    if (!range_valid(b->first_sequence, b->count + 1)) return PW_D3D9_BATCH_INVALID;
    if (pw_d3d9_command_encode(command, sizeof(command), &bytes, q) != PW_D3D9_COMMAND_OK)
        return PW_D3D9_BATCH_INVALID;
    size_t record = (bytes + 8 + 7) & ~(size_t)7;
    if (record > PW_D3D9_BATCH_STORAGE - b->used) return PW_D3D9_BATCH_FULL;
    unsigned char *p = b->records + b->used;
    memset(p, 0, record); put32(p, (uint32_t)bytes); memcpy(p + 8, command, bytes);
    b->offsets[b->count++] = b->used; b->used += (uint32_t)record;
    return PW_D3D9_BATCH_OK;
}
/* Validate the entire immutable record area before exposing any command. */
static int scan(const unsigned char *p, uint32_t used, uint32_t count, uint32_t *offsets)
{
    uint32_t offset = 0;
    if (used > PW_D3D9_BATCH_STORAGE || (used & 7)) return 0;
    for (uint32_t n = 0; n < count; ++n) {
        if (offset > used || used - offset < 8) return 0;
        uint32_t bytes = get32(p + offset);
        if (get32(p + offset + 4) || bytes > PW_D3D9_COMMAND_MAX || bytes > used - offset - 8)
            return 0;
        uint32_t record = (bytes + 15) & ~7u;
        if (record > used - offset ||
            pw_d3d9_command_validate(p + offset + 8, bytes) != PW_D3D9_COMMAND_OK)
            return 0;
        for (uint32_t i = 8 + bytes; i < record; ++i) if (p[offset + i]) return 0;
        offsets[n] = offset; offset += record;
    }
    return offset == used;
}
int pw_d3d9_batch_encode(void *output, size_t capacity, size_t *written, const struct pw_d3d9_command_batch *b)
{
    uint32_t offsets[PW_D3D9_BATCH_LIMIT];
    if (!output || !written || !b || !range_valid(b->first_sequence, b->count) ||
        !scan(b->records, b->used, b->count, offsets) ||
        memcmp(offsets, b->offsets, b->count * sizeof(offsets[0]))) return PW_D3D9_BATCH_INVALID;
    size_t total = PW_D3D9_BATCH_HEADER + b->used;
    if (capacity < total) return PW_D3D9_BATCH_FULL;
    unsigned char *p = output;
    memset(p, 0, PW_D3D9_BATCH_HEADER); put32(p, PW_D3D9_BATCH_VERSION);
    put32(p + 4, b->count); put64(p + 8, b->first_sequence); put32(p + 16, (uint32_t)total);
    memcpy(p + PW_D3D9_BATCH_HEADER, b->records, b->used); *written = total;
    return PW_D3D9_BATCH_OK;
}
int pw_d3d9_batch_decode(struct pw_d3d9_command_batch *output, const void *input, size_t bytes)
{
    const unsigned char *p = input; struct pw_d3d9_command_batch b;
    if (!output || !p || bytes < PW_D3D9_BATCH_HEADER || bytes > PW_D3D9_BATCH_MAX ||
        get32(p) != PW_D3D9_BATCH_VERSION || get32(p + 16) != bytes ||
        get32(p + 20) || get32(p + 24) || get32(p + 28)) return PW_D3D9_BATCH_INVALID;
    memset(&b, 0, sizeof(b)); b.first_sequence = get64(p + 8); b.count = get32(p + 4);
    b.used = (uint32_t)(bytes - PW_D3D9_BATCH_HEADER);
    memcpy(b.records, p + PW_D3D9_BATCH_HEADER, b.used);
    if (!range_valid(b.first_sequence, b.count) ||
        !scan(b.records, b.used, b.count, b.offsets)) return PW_D3D9_BATCH_INVALID;
    *output = b;
    return PW_D3D9_BATCH_OK;
}
int pw_d3d9_batch_command(struct pw_d3d9_command *output, const struct pw_d3d9_command_batch *b, uint32_t index)
{
    if (!output || !b || !range_valid(b->first_sequence, b->count) || index >= b->count ||
        b->used > PW_D3D9_BATCH_STORAGE) return PW_D3D9_BATCH_INVALID;
    uint32_t offset = b->offsets[index];
    if (offset > b->used || b->used - offset < 8) return PW_D3D9_BATCH_INVALID;
    uint32_t bytes = get32(b->records + offset);
    if (get32(b->records + offset + 4) || bytes > b->used - offset - 8 ||
        pw_d3d9_command_decode(output, b->records + offset + 8, bytes) != PW_D3D9_COMMAND_OK)
        return PW_D3D9_BATCH_INVALID;
    return PW_D3D9_BATCH_OK;
}
static int reply_valid(const struct pw_d3d9_batch_reply *r)
{
    if (!r || !range_valid(r->first_sequence, r->count)) return 0;
    if (!(r->hresult & 0x80000000u))
        return !r->hresult && r->attempted == r->count && r->failed_index == UINT32_MAX;
    return r->attempted && r->attempted <= r->count && r->failed_index == r->attempted - 1;
}
int pw_d3d9_batch_reply_encode(void *output, size_t bytes, const struct pw_d3d9_batch_reply *r)
{
    if (!output || bytes < PW_D3D9_BATCH_REPLY || !reply_valid(r)) return PW_D3D9_BATCH_INVALID;
    unsigned char *p = output; memset(p, 0, PW_D3D9_BATCH_REPLY);
    put32(p, PW_D3D9_BATCH_VERSION); put32(p + 4, r->count); put64(p + 8, r->first_sequence);
    put32(p + 16, r->attempted); put32(p + 20, r->failed_index); put32(p + 24, r->hresult);
    return PW_D3D9_BATCH_OK;
}
int pw_d3d9_batch_reply_decode(struct pw_d3d9_batch_reply *output, const void *input, size_t bytes,
                             uint64_t first, uint32_t count)
{
    const unsigned char *p = input;
    if (!output || !p || bytes != PW_D3D9_BATCH_REPLY || get32(p) != PW_D3D9_BATCH_VERSION || get32(p + 28))
        return PW_D3D9_BATCH_INVALID;
    struct pw_d3d9_batch_reply r = {get64(p + 8), get32(p + 4), get32(p + 16), get32(p + 20), get32(p + 24)};
    if (!reply_valid(&r) || r.first_sequence != first || r.count != count) return PW_D3D9_BATCH_INVALID;
    *output = r; return PW_D3D9_BATCH_OK;
}
