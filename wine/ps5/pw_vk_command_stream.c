/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_vk_command_stream.h"
#include <string.h>

static int valid_range(const void *data, size_t bytes)
{
    return (!bytes || data) && bytes <= UINTPTR_MAX - (uintptr_t)data;
}
static int overlap(const void *a, size_t an, const void *b, size_t bn)
{
    return an && bn && (uintptr_t)a < (uintptr_t)b + bn && (uintptr_t)b < (uintptr_t)a + an;
}
static uint32_t load32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t load64(const unsigned char *p)
{
    return load32(p) | (uint64_t)load32(p + 4) << 32;
}
static void store32(unsigned char *p, uint32_t n)
{
    unsigned i;
    for (i = 0; i < 4; ++i) p[i] = (unsigned char)(n >> (8 * i));
}
static void store64(unsigned char *p, uint64_t n)
{
    store32(p, (uint32_t)n); store32(p + 4, (uint32_t)(n >> 32));
}
static int decode(const unsigned char *data, size_t bytes,
                  struct pw_vk_stream_record *record, size_t *size)
{
    size_t i;
    uint32_t total, payload;
    if (bytes < PW_VK_STREAM_HEADER) return PW_VK_STREAM_INVALID;
    total = load32(data + 8); payload = load32(data + 16);
    if (load32(data) != PW_VK_STREAM_MAGIC || load32(data + 4) != PW_VK_STREAM_VERSION ||
        load32(data + 20) || !load32(data + 12) || !load64(data + 24) ||
        total < PW_VK_STREAM_HEADER || total > bytes || (total & 7) ||
        payload > total - PW_VK_STREAM_HEADER || total - PW_VK_STREAM_HEADER - payload >= 8)
        return PW_VK_STREAM_INVALID;
    for (i = PW_VK_STREAM_HEADER + (size_t)payload; i < total; ++i)
        if (data[i]) return PW_VK_STREAM_INVALID;
    record->sequence = load64(data + 24); record->opcode = load32(data + 12);
    record->payload_bytes = payload; record->payload = data + PW_VK_STREAM_HEADER;
    *size = total;
    return PW_VK_STREAM_OK;
}
void pw_vk_stream_registry_init(struct pw_vk_stream_registry *registry)
{
    registry->streams = NULL; registry->next_sequence = 1;
}
static int registered(const struct pw_vk_stream_registry *registry, const struct pw_vk_stream *stream)
{
    const struct pw_vk_stream *s;
    for (s = registry->streams; s; s = s->next) if (s == stream) return 1;
    return 0;
}
int pw_vk_stream_register(struct pw_vk_stream_registry *registry, struct pw_vk_stream *stream,
                          void *arena, size_t capacity)
{
    struct pw_vk_stream *s;
    if (!registry || !stream || stream->owner || !arena || capacity < PW_VK_STREAM_HEADER ||
        !valid_range(arena, capacity) || registered(registry, stream))
        return PW_VK_STREAM_INVALID;
    for (s = registry->streams; s; s = s->next)
        if (overlap(arena, capacity, s->arena, s->capacity)) return PW_VK_STREAM_INVALID;
    stream->owner = registry; stream->arena = arena; stream->capacity = capacity; stream->used = stream->cursor = 0;
    stream->next = registry->streams; registry->streams = stream;
    return PW_VK_STREAM_OK;
}
int pw_vk_stream_unregister(struct pw_vk_stream_registry *registry, struct pw_vk_stream *stream)
{
    struct pw_vk_stream **s;
    if (!registry || !stream) return PW_VK_STREAM_INVALID;
    for (s = &registry->streams; *s; s = &(*s)->next)
        if (*s == stream) {
            if (stream->used) return PW_VK_STREAM_PENDING;
            *s = stream->next; stream->next = NULL; stream->owner = NULL;
            return PW_VK_STREAM_OK;
        }
    return PW_VK_STREAM_INVALID;
}
int pw_vk_stream_append(struct pw_vk_stream_registry *registry, struct pw_vk_stream *stream,
                        uint32_t opcode, const void *payload, uint32_t payload_bytes)
{
    size_t total;
    unsigned char *dest;
    if (!registry || !stream || stream->owner != registry || !opcode ||
        !valid_range(payload, payload_bytes) || payload_bytes > UINT32_MAX - PW_VK_STREAM_HEADER - 7)
        return PW_VK_STREAM_INVALID;
    total = (PW_VK_STREAM_HEADER + (size_t)payload_bytes + 7) & ~(size_t)7;
    if (stream->used > stream->capacity || total > stream->capacity - stream->used) return PW_VK_STREAM_FULL;
    /* Keep UINT64_MAX as a sentinel; never wrap and reorder outstanding work. */
    if (!registry->next_sequence || registry->next_sequence == UINT64_MAX) return PW_VK_STREAM_SEQUENCE_EXHAUSTED;
    dest = stream->arena + stream->used;
    if (overlap(dest, total, payload, payload_bytes)) return PW_VK_STREAM_INVALID;
    memset(dest, 0, total);
    store32(dest, PW_VK_STREAM_MAGIC); store32(dest + 4, PW_VK_STREAM_VERSION);
    store32(dest + 8, (uint32_t)total); store32(dest + 12, opcode);
    store32(dest + 16, payload_bytes); store64(dest + 24, registry->next_sequence++);
    if (payload_bytes) memcpy(dest + PW_VK_STREAM_HEADER, payload, payload_bytes);
    stream->used += total;
    return PW_VK_STREAM_OK;
}
int pw_vk_stream_validate(const void *batch, size_t bytes, size_t *records)
{
    const unsigned char *data = batch;
    struct pw_vk_stream_record record;
    size_t cursor = 0, size, count = 0;
    uint64_t previous = 0;
    if (!records || !valid_range(batch, bytes)) return PW_VK_STREAM_INVALID;
    *records = 0;
    while (cursor < bytes) {
        if (decode(data + cursor, bytes - cursor, &record, &size) || record.sequence <= previous)
            return PW_VK_STREAM_INVALID;
        previous = record.sequence; cursor += size; ++count;
    }
    *records = count;
    return PW_VK_STREAM_OK;
}
int pw_vk_stream_collect(struct pw_vk_stream_registry *registry, void *scratch,
                         size_t capacity, size_t *bytes, size_t *records)
{
    struct pw_vk_stream *s, *best;
    struct pw_vk_stream_record record;
    size_t total = 0, written = 0, count = 0, size, best_size;
    uint64_t sequence, previous = 0;
    if (!registry || !bytes || !records || !valid_range(scratch, capacity)) return PW_VK_STREAM_INVALID;
    *bytes = *records = 0;
    for (s = registry->streams; s; s = s->next) {
        size_t n;
        if (overlap(scratch, capacity, s->arena, s->capacity)) return PW_VK_STREAM_INVALID;
        if (s->used > s->capacity || pw_vk_stream_validate(s->arena, s->used, &n)) return PW_VK_STREAM_INVALID;
        if (s->used > capacity - total) return PW_VK_STREAM_FULL;
        total += s->used; s->cursor = 0;
    }
    /* Each arena is already sorted. Merge fronts without reordering records
     * from different threads, e.g. descriptor update A -> bind B -> draw A. */
    while (written < total) {
        best = NULL; best_size = 0; sequence = UINT64_MAX;
        for (s = registry->streams; s; s = s->next) {
            if (s->cursor == s->used) continue;
            if (decode(s->arena + s->cursor, s->used - s->cursor, &record, &size)) return PW_VK_STREAM_INVALID;
            if (record.sequence < sequence) { best = s; best_size = size; sequence = record.sequence; }
        }
        if (!best || sequence <= previous || sequence >= registry->next_sequence) return PW_VK_STREAM_INVALID;
        memcpy((unsigned char *)scratch + written, best->arena + best->cursor, best_size);
        best->cursor += best_size; written += best_size; ++count; previous = sequence;
    }
    for (s = registry->streams; s; s = s->next) s->used = s->cursor = 0;
    *bytes = written; *records = count;
    return PW_VK_STREAM_OK;
}
int pw_vk_stream_replay(const void *batch, size_t bytes, pw_vk_stream_replay_fn callback,
                        void *context, size_t *completed)
{
    const unsigned char *data = batch;
    struct pw_vk_stream_record record;
    size_t cursor = 0, count, size;
    if (!callback || !completed) return PW_VK_STREAM_INVALID;
    *completed = 0;
    if (pw_vk_stream_validate(batch, bytes, &count)) return PW_VK_STREAM_INVALID;
    while (cursor < bytes) {
        if (decode(data + cursor, bytes - cursor, &record, &size)) return PW_VK_STREAM_INVALID;
        if (callback(context, &record)) return PW_VK_STREAM_REPLAY_FAILED;
        ++*completed; cursor += size;
    }
    return PW_VK_STREAM_OK;
}
