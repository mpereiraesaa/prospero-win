/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_vk_command_stream.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

struct replay_state { unsigned count, fail_at; uint64_t sequence; unsigned expected[8]; };
static int replay(void *context, const struct pw_vk_stream_record *r)
{
    struct replay_state *s = context;
    unsigned value;
    assert(r->sequence > s->sequence);
    assert(r->payload_bytes == sizeof(value));
    memcpy(&value, r->payload, sizeof(value));
    if (s->count < 3) assert(value == s->expected[s->count]);
    if (s->fail_at && s->count + 1 == s->fail_at) return 1;
    s->sequence = r->sequence; ++s->count;
    return 0;
}
static void ownership_order_and_lifecycle(void)
{
    struct pw_vk_stream_registry registry, second;
    struct pw_vk_stream a = {0}, b = {0};
    unsigned char aa[160], ba[160], output[320], corrupt[320];
    size_t bytes, records, completed;
    unsigned value;
    struct replay_state state = {0, 0, 0, {11, 22, 33}};
    pw_vk_stream_registry_init(&registry); pw_vk_stream_registry_init(&second);
    assert(!pw_vk_stream_register(&registry, &a, aa, sizeof(aa)));
    assert(!pw_vk_stream_register(&registry, &b, ba, sizeof(ba)));
    assert(pw_vk_stream_register(&second, &a, aa, sizeof(aa)) == PW_VK_STREAM_INVALID);
    { struct pw_vk_stream alias = {0};
      assert(pw_vk_stream_register(&registry, &alias, aa + 1, sizeof(aa) - 1) == PW_VK_STREAM_INVALID); }
    value = 11;
    assert(pw_vk_stream_append(&second, &a, 1, &value, sizeof(value)) == PW_VK_STREAM_INVALID);
    assert(second.next_sequence == 1 && a.used == 0);
    assert(!pw_vk_stream_append(&registry, &a, 1, &value, sizeof(value)));
    value = 22; assert(!pw_vk_stream_append(&registry, &b, 2, &value, sizeof(value)));
    value = 33; assert(!pw_vk_stream_append(&registry, &a, 3, &value, sizeof(value)));
    value = 99; /* Caller storage reuse cannot affect queued data. */
    assert(pw_vk_stream_unregister(&registry, &a) == PW_VK_STREAM_PENDING);
    assert(pw_vk_stream_collect(&registry, output, 119, &bytes, &records) == PW_VK_STREAM_FULL);
    assert(a.used == 80 && b.used == 40 && bytes == 0 && records == 0);
    assert(pw_vk_stream_collect(&registry, aa, sizeof(aa), &bytes, &records) == PW_VK_STREAM_INVALID);
    assert(!pw_vk_stream_collect(&registry, output, sizeof(output), &bytes, &records));
    assert(bytes == 120 && records == 3 && a.used == 0 && b.used == 0);
    /* Golden framing bytes are identical for 32/64-bit producers. */
    assert(!memcmp(output, "\x4b\x56\x57\x50\x01\0\0\0\x28\0\0\0\x01\0\0\0\x04\0\0\0\0\0\0\0\x01\0\0\0\0\0\0\0", 32));
    assert(!pw_vk_stream_replay(output, bytes, replay, &state, &completed));
    assert(completed == 3 && state.count == 3);
    /* Full-batch framing validation precedes any side effect. */
    memcpy(corrupt, output, bytes); corrupt[80 + 4] = 2;
    state.count = 0; state.sequence = 0;
    assert(pw_vk_stream_replay(corrupt, bytes, replay, &state, &completed) == PW_VK_STREAM_INVALID);
    assert(completed == 0 && state.count == 0);
    memcpy(corrupt, output, bytes); corrupt[39] = 1; /* Nonzero padding. */
    assert(pw_vk_stream_validate(corrupt, bytes, &records) == PW_VK_STREAM_INVALID);
    memcpy(corrupt, output, bytes); corrupt[40 + 24] = 1; /* Duplicate sequence. */
    assert(pw_vk_stream_validate(corrupt, bytes, &records) == PW_VK_STREAM_INVALID);
    assert(pw_vk_stream_validate(output, bytes - 1, &records) == PW_VK_STREAM_INVALID);
    /* A failed callback reports the exact completed prefix, without retry. */
    state.count = 0; state.sequence = 0; state.fail_at = 2;
    assert(pw_vk_stream_replay(output, bytes, replay, &state, &completed) == PW_VK_STREAM_REPLAY_FAILED);
    assert(completed == 1 && state.count == 1);
    assert(!pw_vk_stream_unregister(&registry, &a)); assert(!a.owner);
    assert(!pw_vk_stream_register(&second, &a, aa, sizeof(aa)));
    assert(!pw_vk_stream_unregister(&second, &a));
    assert(!pw_vk_stream_unregister(&registry, &b));
}
static void capacity_and_malformed_collect(void)
{
    struct pw_vk_stream_registry registry;
    struct pw_vk_stream a = {0}, b = {0};
    unsigned char aa[40], ba[40], output[80];
    size_t bytes, records;
    unsigned value = 7;
    pw_vk_stream_registry_init(&registry);
    assert(!pw_vk_stream_register(&registry, &a, aa, sizeof(aa)));
    assert(!pw_vk_stream_register(&registry, &b, ba, sizeof(ba)));
    assert(pw_vk_stream_append(&registry, &a, 1, aa, 4) == PW_VK_STREAM_INVALID);
    assert(!pw_vk_stream_append(&registry, &a, 1, &value, 4));
    assert(pw_vk_stream_append(&registry, &a, 1, &value, 4) == PW_VK_STREAM_FULL);
    assert(registry.next_sequence == 2);
    assert(!pw_vk_stream_append(&registry, &b, 1, &value, 4));
    ba[4] = 2;
    assert(pw_vk_stream_collect(&registry, output, sizeof(output), &bytes, &records) == PW_VK_STREAM_INVALID);
    assert(a.used == 40 && b.used == 40);
    ba[4] = 1; ba[24] = 1;
    assert(pw_vk_stream_collect(&registry, output, sizeof(output), &bytes, &records) == PW_VK_STREAM_INVALID);
    assert(a.used == 40 && b.used == 40); /* Merge error also preserves work. */
    ba[24] = 2;
    assert(!pw_vk_stream_collect(&registry, output, sizeof(output), &bytes, &records));
    registry.next_sequence = UINT64_MAX;
    assert(pw_vk_stream_append(&registry, &a, 1, &value, 4) == PW_VK_STREAM_SEQUENCE_EXHAUSTED);
    assert(a.used == 0);
    assert(pw_vk_stream_append(&registry, &a, 1, &value, UINT32_MAX) == PW_VK_STREAM_INVALID);
    assert(pw_vk_stream_append(&registry, &a, 0, &value, 4) == PW_VK_STREAM_INVALID);
    assert(pw_vk_stream_append(&registry, &a, 1, NULL, 4) == PW_VK_STREAM_INVALID);
    assert(!pw_vk_stream_collect(&registry, NULL, 0, &bytes, &records));
    assert(bytes == 0 && records == 0);
}

static struct pw_vk_stream_registry concurrent;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
struct worker { struct pw_vk_stream stream; unsigned char arena[40000]; unsigned id; };
static void *producer(void *arg)
{
    struct worker *worker = arg;
    unsigned i;
    for (i = 0; i < 1000; ++i) {
        unsigned value = worker->id * 1000 + i;
        assert(!pthread_mutex_lock(&lock));
        assert(!pw_vk_stream_append(&concurrent, &worker->stream, 1, &value, sizeof(value)));
        assert(!pthread_mutex_unlock(&lock));
    }
    return NULL;
}
struct concurrent_result { unsigned char seen[2000]; unsigned count; uint64_t sequence; };
static int concurrent_replay(void *arg, const struct pw_vk_stream_record *r)
{
    struct concurrent_result *result = arg;
    unsigned value;
    memcpy(&value, r->payload, sizeof(value));
    assert(value < 2000 && !result->seen[value]);
    assert(r->sequence == result->sequence + 1);
    result->sequence = r->sequence; result->seen[value] = 1; ++result->count;
    return 0;
}
static void concurrent_producers(void)
{
    struct worker a = { .id = 0 }, b = { .id = 1 };
    struct concurrent_result result = {0};
    unsigned char output[80000]; pthread_t threads[2]; size_t bytes, records, completed;
    pw_vk_stream_registry_init(&concurrent);
    assert(!pw_vk_stream_register(&concurrent, &a.stream, a.arena, sizeof(a.arena)));
    assert(!pw_vk_stream_register(&concurrent, &b.stream, b.arena, sizeof(b.arena)));
    assert(!pthread_create(&threads[0], NULL, producer, &a));
    assert(!pthread_create(&threads[1], NULL, producer, &b));
    assert(!pthread_join(threads[0], NULL)); assert(!pthread_join(threads[1], NULL));
    assert(!pw_vk_stream_collect(&concurrent, output, sizeof(output), &bytes, &records));
    assert(records == 2000 && bytes == sizeof(output));
    assert(!pw_vk_stream_replay(output, bytes, concurrent_replay, &result, &completed));
    assert(completed == 2000 && result.count == 2000 && result.sequence == 2000);
}
int main(void)
{
    ownership_order_and_lifecycle(); capacity_and_malformed_collect(); concurrent_producers();
    puts("PASS command stream ownership, global order, capacity, malformed preflight, lifecycle and concurrent producers");
    return 0;
}
