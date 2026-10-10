/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_VK_COMMAND_STREAM_H
#define PW_VK_COMMAND_STREAM_H
#include <stddef.h>
#include <stdint.h>

/* Versioned, little-endian record framing without embedded framing pointers. Vulkan adapters
 * must normalize/deep-copy payloads before append; borrowed pointers are
 * never a supported payload representation. */
#define PW_VK_STREAM_VERSION 1u
#define PW_VK_STREAM_HEADER 32u
#define PW_VK_STREAM_MAGIC UINT32_C(0x5057564b)
enum pw_vk_stream_status {
    PW_VK_STREAM_OK, PW_VK_STREAM_INVALID, PW_VK_STREAM_FULL,
    PW_VK_STREAM_PENDING, PW_VK_STREAM_SEQUENCE_EXHAUSTED,
    PW_VK_STREAM_REPLAY_FAILED
};
struct pw_vk_stream {
    struct pw_vk_stream *next;
    struct pw_vk_stream_registry *owner;
    unsigned char *arena;
    size_t capacity, used, cursor;
};
struct pw_vk_stream_registry {
    struct pw_vk_stream *streams;
    uint64_t next_sequence;
};
struct pw_vk_stream_record {
    uint64_t sequence;
    uint32_t opcode, payload_bytes;
    const unsigned char *payload;
};
typedef int (*pw_vk_stream_replay_fn)(void *, const struct pw_vk_stream_record *);

/* Registry operations require caller serialization across all producers.
 * No locks, allocation, TLS or guest/host crossing occurs in this core. */
void pw_vk_stream_registry_init(struct pw_vk_stream_registry *);
/* Stream nodes must be zero-initialized and arenas must not overlap. */
int pw_vk_stream_register(struct pw_vk_stream_registry *, struct pw_vk_stream *,
                          void *arena, size_t capacity);
int pw_vk_stream_unregister(struct pw_vk_stream_registry *, struct pw_vk_stream *);
int pw_vk_stream_append(struct pw_vk_stream_registry *, struct pw_vk_stream *,
                        uint32_t opcode, const void *payload, uint32_t payload_bytes);
/* Drain all registered streams in global append order into owned scratch.
 * Scratch must not overlap any registered arena.
 * Failure preserves all pending records. Success clears every stream.
 * The adapter keeps scratch alive and serializes flush/replay until the
 * synchronous Unix replay finishes. A replay failure is fatal: do not retry
 * the drained batch or fall back to resubmitting its records. */
int pw_vk_stream_collect(struct pw_vk_stream_registry *, void *scratch,
                         size_t capacity, size_t *bytes, size_t *records);
/* Callback must not mutate/retain batch storage, recurse into replay, or
 * resubmit a failed record. Validate the entire batch before the first callback. Per-opcode payload
 * validation is the adapter's separate preflight responsibility. */
int pw_vk_stream_validate(const void *batch, size_t bytes, size_t *records);
int pw_vk_stream_replay(const void *batch, size_t bytes, pw_vk_stream_replay_fn,
                        void *context, size_t *completed);
#endif
