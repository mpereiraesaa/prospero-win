/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_command_batch.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void put32(unsigned char *p, uint32_t v)
{ for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i)); }
static void rejected(const unsigned char *wire, size_t size)
{
    struct pw_d3d9_command_batch out, before;
    memset(&out, 0xa5, sizeof(out)); before = out;
    assert(pw_d3d9_batch_decode(&out, wire, size) == PW_D3D9_BATCH_INVALID);
    assert(!memcmp(&out, &before, sizeof(out)));
}
int main(void)
{
    struct pw_d3d9_command_batch b, out, saved;
    struct pw_d3d9_command q = {.method = 57, .args = {7, 1}}, command;
    unsigned char wire[PW_D3D9_BATCH_MAX + 1], mutated[sizeof(wire)], sentinel[sizeof(wire)];
    size_t size = 99, written = 73;
    memset(&b, 0xa6, sizeof(b)); saved = b;
    assert(pw_d3d9_batch_init(&b, 0)); assert(!memcmp(&b, &saved, sizeof(b)));
    assert(!pw_d3d9_batch_init(&b, UINT64_C(0x100000001)));
    assert(pw_d3d9_batch_encode(wire, sizeof(wire), &size, &b)); assert(size == 99);
    assert(!pw_d3d9_batch_append(&b, &q)); q.args[1] = 55;
    assert(!pw_d3d9_batch_command(&command, &b, 0) && command.args[1] == 1);
    q = (struct pw_d3d9_command){.method = 94, .args = {2, 2}, .data_bytes = 32};
    for (unsigned i = 0; i < 32; ++i) q.data.bytes[i] = (unsigned char)(i ^ 0xa5);
    assert(!pw_d3d9_batch_append(&b, &q)); memset(q.data.bytes, 0, 32);
    assert(!pw_d3d9_batch_encode(wire, sizeof(wire), &size, &b));
    assert(!pw_d3d9_batch_decode(&out, wire, size)); assert(!memcmp(&b, &out, sizeof(b)));
    assert(!pw_d3d9_batch_command(&command, &out, 1));
    for (unsigned i = 0; i < 32; ++i) assert(command.data.bytes[i] == (unsigned char)(i ^ 0xa5));
    memcpy(mutated, wire, size); memset(wire, 0, size);
    assert(!pw_d3d9_batch_command(&command, &out, 1));
    for (unsigned i = 0; i < 32; ++i) assert(command.data.bytes[i] == (unsigned char)(i ^ 0xa5));
    memcpy(wire, mutated, size);
    for (size_t i = 0; i < size; ++i) rejected(wire, i);
    wire[size] = 0; rejected(wire, size + 1);
    const unsigned fields[] = {0, 4, 16, 20, 24, 28, 32, 36, 40};
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        memcpy(mutated, wire, size); put32(mutated + fields[i], UINT32_MAX); rejected(mutated, size);
    }
    memcpy(mutated, wire, size); memset(mutated + 8, 0, 8); rejected(mutated, size);
    memcpy(mutated, wire, size); memset(mutated + 8, 0xff, 8); rejected(mutated, size);
    memcpy(mutated, wire, size); put32(mutated + 32 + b.offsets[1] + 8, 999); rejected(mutated, size);
    saved = b; b.offsets[1]++; memset(mutated, 0x9b, sizeof(mutated)); memcpy(sentinel, mutated, sizeof(mutated));
    assert(pw_d3d9_batch_encode(mutated, sizeof(mutated), &written, &b));
    assert(written == 73 && !memcmp(mutated, sentinel, sizeof(mutated))); b = saved;
    assert(pw_d3d9_batch_encode(mutated, size - 1, &written, &b) == PW_D3D9_BATCH_FULL);
    assert(written == 73 && !memcmp(mutated, sentinel, sizeof(mutated)));
    /* One BOOL constant has padding after its 28-byte command payload. */
    assert(!pw_d3d9_batch_init(&b, 9)); q = (struct pw_d3d9_command){.method = 98, .args = {0, 1}, .data_bytes = 4};
    q.data.words[0] = 1; assert(!pw_d3d9_batch_append(&b, &q));
    assert(!pw_d3d9_batch_encode(wire, sizeof(wire), &size, &b));
    assert(!(size & 7)); wire[size - 1] = 1; rejected(wire, size);
    /* Invalid canonical BOOL data in a suffix is rejected by every structural
     * scan before exposing a prefix; encode leaves output/length untouched. */
    wire[size - 1] = 0;
    put32(wire + PW_D3D9_BATCH_HEADER + 8 + 24, 2); rejected(wire, size);
    assert(!pw_d3d9_batch_init(&b, 11));
    q = (struct pw_d3d9_command){.method = 57, .args = {7, 1}};
    assert(!pw_d3d9_batch_append(&b, &q));
    q = (struct pw_d3d9_command){.method = 98, .args = {0, 1}, .data_bytes = 4};
    assert(!pw_d3d9_batch_append(&b, &q));
    assert(!pw_d3d9_batch_encode(wire, sizeof(wire), &size, &b));
    put32(wire + PW_D3D9_BATCH_HEADER + b.offsets[1] + 8 + 24, 2); rejected(wire, size);
    put32(b.records + b.offsets[1] + 8 + 24, 2);
    memset(mutated, 0x9b, sizeof(mutated)); memcpy(sentinel, mutated, sizeof(mutated)); written = 73;
    assert(pw_d3d9_batch_encode(mutated, sizeof(mutated), &written, &b) == PW_D3D9_BATCH_INVALID);
    assert(written == 73 && !memcmp(mutated, sentinel, sizeof(mutated)));
    struct pw_d3d9_command command_before;
    memset(&command, 0xa7, sizeof(command)); command_before = command;
    assert(pw_d3d9_batch_command(&command, &b, 1) == PW_D3D9_BATCH_INVALID);
    assert(!memcmp(&command, &command_before, sizeof(command)));
    /* Full builders and sequence exhaustion remain unchanged. */
    assert(!pw_d3d9_batch_init(&b, 17)); q = (struct pw_d3d9_command){.method = 57, .args = {7, 1}};
    for (unsigned i = 0; i < PW_D3D9_BATCH_LIMIT; ++i) assert(!pw_d3d9_batch_append(&b, &q));
    saved = b; assert(pw_d3d9_batch_append(&b, &q) == PW_D3D9_BATCH_FULL); assert(!memcmp(&b, &saved, sizeof(b)));
    assert(!pw_d3d9_batch_encode(wire, sizeof(wire), &size, &b)); assert(!pw_d3d9_batch_decode(&out, wire, size));
    assert(!pw_d3d9_batch_init(&b, UINT64_MAX)); assert(!pw_d3d9_batch_append(&b, &q)); saved = b;
    assert(pw_d3d9_batch_append(&b, &q) == PW_D3D9_BATCH_INVALID); assert(!memcmp(&b, &saved, sizeof(b)));
    assert(!pw_d3d9_batch_encode(wire, sizeof(wire), &size, &b)); assert(!pw_d3d9_batch_decode(&out, wire, size));
    assert(!pw_d3d9_batch_init(&b, 1));
    q = (struct pw_d3d9_command){.method = 94, .args = {0, 256}, .data_bytes = 4096};
    assert(!pw_d3d9_batch_append(&b, &q)); saved = b;
    assert(pw_d3d9_batch_append(&b, &q) == PW_D3D9_BATCH_FULL); assert(!memcmp(&b, &saved, sizeof(b)));
    q.method = 999; assert(pw_d3d9_batch_append(&b, &q) == PW_D3D9_BATCH_INVALID); assert(!memcmp(&b, &saved, sizeof(b)));
    /* Every first-failure position, plus correlated complete-success replies. */
    for (unsigned count = 1; count <= PW_D3D9_BATCH_LIMIT; ++count) {
        for (unsigned attempted = 1; attempted <= count; ++attempted) {
            struct pw_d3d9_batch_reply r = {UINT64_C(0x100000001), count, attempted, attempted - 1, 0x88760868}, decoded, before;
            memset(&before, 0xcc, sizeof(before)); decoded = before;
            assert(!pw_d3d9_batch_reply_encode(wire, sizeof(wire), &r));
            assert(!pw_d3d9_batch_reply_decode(&decoded, wire, 32, r.first_sequence, count));
            assert(decoded.first_sequence == r.first_sequence && decoded.count == count && decoded.attempted == attempted && decoded.failed_index == attempted - 1 && decoded.hresult == r.hresult);
            decoded = before; assert(pw_d3d9_batch_reply_decode(&decoded, wire, 32, r.first_sequence + 1, count)); assert(!memcmp(&decoded, &before, sizeof(decoded)));
            assert(pw_d3d9_batch_reply_decode(&decoded, wire, 32, r.first_sequence, count + 1));
            r.hresult = 0; r.attempted = count; r.failed_index = UINT32_MAX;
            assert(!pw_d3d9_batch_reply_encode(wire, sizeof(wire), &r));
            assert(!pw_d3d9_batch_reply_decode(&decoded, wire, 32, r.first_sequence, count));
            for (size_t n = 0; n < 32; ++n) assert(pw_d3d9_batch_reply_decode(&decoded, wire, n, r.first_sequence, count));
            wire[28] = 1; assert(pw_d3d9_batch_reply_decode(&decoded, wire, 32, r.first_sequence, count));
            r.failed_index = 0; assert(pw_d3d9_batch_reply_encode(wire, sizeof(wire), &r));
            r.hresult = 1; r.failed_index = UINT32_MAX; assert(pw_d3d9_batch_reply_encode(wire, sizeof(wire), &r));
            r.hresult = 0x80004005; r.attempted = 0; r.failed_index = UINT32_MAX; assert(pw_d3d9_batch_reply_encode(wire, sizeof(wire), &r));
        }
    }
    puts("command batch wire passed"); return 0;
}
