/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_vk_spsc.h"
#include <string.h>

static int range(const void *p, size_t n)
{
    return (!n || p) && n <= UINTPTR_MAX - (uintptr_t)p;
}
static int overlaps(const void *a, size_t an, const void *b, size_t bn)
{
    return an && bn && (uintptr_t)a < (uintptr_t)b + bn && (uintptr_t)b < (uintptr_t)a + an;
}
static void put32(unsigned char *p, uint32_t v)
{
    unsigned i;
    for (i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i));
}
static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void copy_in(struct pw_vk_spsc *s, uint64_t at, const void *src, size_t n)
{
    size_t pos = (size_t)at & (s->capacity - 1), first = s->capacity - pos;
    if (first > n) first = n;
    if (first) memcpy(s->arena + pos, src, first);
    if (n > first) memcpy(s->arena, (const unsigned char *)src + first, n - first);
}
static void copy_out(struct pw_vk_spsc *s, uint64_t at, void *dst, size_t n)
{
    size_t pos = (size_t)at & (s->capacity - 1), first = s->capacity - pos;
    if (first > n) first = n;
    if (first) memcpy(dst, s->arena + pos, first);
    if (n > first) memcpy((unsigned char *)dst + first, s->arena, n - first);
}
void pw_vk_spsc_sequence_init(struct pw_vk_spsc_sequence *s)
{
    atomic_init(&s->next, 1);
}
int pw_vk_spsc_init(struct pw_vk_spsc *s, void *arena, size_t capacity)
{
    if (!s || !arena || capacity < PW_VK_STREAM_HEADER || capacity > UINT32_MAX || (capacity & (capacity - 1)) ||
        !range(arena, capacity) || overlaps(s, sizeof(*s), arena, capacity))
        return PW_VK_STREAM_INVALID;
    atomic_init(&s->read, 0); atomic_init(&s->write, 0);
    /* Never silently use a library mutex on a target without native atomics. */
    if (!atomic_is_lock_free(&s->read) || !atomic_is_lock_free(&s->write))
        return PW_VK_STREAM_INVALID;
    s->arena = arena; s->capacity = capacity;
    return PW_VK_STREAM_OK;
}
uint64_t pw_vk_spsc_marker(const struct pw_vk_spsc_sequence *s)
{
    return atomic_load_explicit(&s->next, memory_order_acquire) - 1;
}
int pw_vk_spsc_append(struct pw_vk_spsc_sequence *seq, struct pw_vk_spsc *s,
                     uint32_t opcode, const void *payload, uint32_t bytes)
{
    unsigned char header[PW_VK_STREAM_HEADER] = {0}, padding[8] = {0};
    uint64_t read, write, ticket;
    size_t total;
    if (!seq || !s || !s->arena || !opcode || !range(payload, bytes) ||
        bytes > UINT32_MAX - PW_VK_STREAM_HEADER - 7 ||
        overlaps(s->arena, s->capacity, payload, bytes) || !atomic_is_lock_free(&seq->next))
        return PW_VK_STREAM_INVALID;
    total = (PW_VK_STREAM_HEADER + (size_t)bytes + 7) & ~(size_t)7;
    write = atomic_load_explicit(&s->write, memory_order_relaxed);
    read = atomic_load_explicit(&s->read, memory_order_acquire);
    if (read > write || write - read > s->capacity) return PW_VK_STREAM_INVALID;
    if (total > s->capacity - (size_t)(write - read)) return PW_VK_STREAM_FULL;
    if (write > UINT64_MAX - total) return PW_VK_STREAM_SEQUENCE_EXHAUSTED;
    ticket = atomic_load_explicit(&seq->next, memory_order_relaxed);
    do {
        if (!ticket || ticket == UINT64_MAX) return PW_VK_STREAM_SEQUENCE_EXHAUSTED;
    } while (!atomic_compare_exchange_weak_explicit(&seq->next, &ticket, ticket + 1,
                                                   memory_order_acq_rel, memory_order_relaxed));
    put32(header, PW_VK_STREAM_MAGIC); put32(header + 4, PW_VK_STREAM_VERSION);
    put32(header + 8, (uint32_t)total); put32(header + 12, opcode);
    put32(header + 16, bytes); put32(header + 24, (uint32_t)ticket);
    put32(header + 28, (uint32_t)(ticket >> 32));
    copy_in(s, write, header, sizeof(header));
    copy_in(s, write + sizeof(header), payload, bytes);
    copy_in(s, write + sizeof(header) + bytes, padding, total - sizeof(header) - bytes);
    atomic_store_explicit(&s->write, write + total, memory_order_release);
    return PW_VK_STREAM_OK;
}
int pw_vk_spsc_take(struct pw_vk_spsc *s, uint64_t expected, void *scratch,
                   size_t capacity, size_t *bytes)
{
    unsigned char header[PW_VK_STREAM_HEADER];
    uint64_t read, write, ticket;
    size_t total, payload;
    if (!s || !s->arena || !expected || !bytes || !range(scratch, capacity) ||
        overlaps(s->arena, s->capacity, scratch, capacity)) return PW_VK_STREAM_INVALID;
    *bytes = 0;
    read = atomic_load_explicit(&s->read, memory_order_relaxed);
    write = atomic_load_explicit(&s->write, memory_order_acquire);
    if (write < read || write - read > s->capacity) return PW_VK_STREAM_INVALID;
    if (write == read) return PW_VK_STREAM_PENDING;
    if (write - read < sizeof(header)) return PW_VK_STREAM_INVALID;
    copy_out(s, read, header, sizeof(header));
    ticket = get32(header + 24) | (uint64_t)get32(header + 28) << 32;
    total = get32(header + 8); payload = get32(header + 16);
    if (get32(header) != PW_VK_STREAM_MAGIC || get32(header + 4) != PW_VK_STREAM_VERSION ||
        !get32(header + 12) || get32(header + 20) || !ticket ||
        total < sizeof(header) || total > write - read || (total & 7) ||
        payload > total - sizeof(header) || total - sizeof(header) - payload >= 8)
        return PW_VK_STREAM_INVALID;
    if (ticket < expected) return PW_VK_STREAM_INVALID;
    if (ticket > expected) return PW_VK_STREAM_PENDING;
    if (total > capacity) return PW_VK_STREAM_FULL;
    copy_out(s, read, scratch, total);
    /* Preserve the legacy framing validation (including zero padding) before
     * releasing bytes. Semantic Vulkan preflight remains the adapter's job. */
    { size_t count; if (pw_vk_stream_validate(scratch, total, &count) || count != 1) return PW_VK_STREAM_INVALID; }
    atomic_store_explicit(&s->read, read + total, memory_order_release);
    *bytes = total;
    return PW_VK_STREAM_OK;
}
