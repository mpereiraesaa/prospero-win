/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_BRIDGE_WIRE_H
#define PW_D3D9_BRIDGE_WIRE_H
#include <stddef.h>
#include <stdint.h>

#define PW_D3D9_WIRE_VERSION 1u
#define PW_D3D9_WIRE_HEADER 64u
#define PW_D3D9_WIRE_MAX_RING (1u << 20)
#define PW_D3D9_WIRE_PENDING 32u

enum pw_d3d9_wire_result {
    PW_D3D9_OK, PW_D3D9_EMPTY, PW_D3D9_FULL, PW_D3D9_SMALL,
    PW_D3D9_CLOSED, PW_D3D9_INVALID
};
enum pw_d3d9_wire_role { PW_D3D9_CLIENT, PW_D3D9_SERVICE };
enum pw_d3d9_wire_state { PW_D3D9_STARTING, PW_D3D9_READY, PW_D3D9_STOPPING, PW_D3D9_STOPPED };
enum pw_d3d9_wire_opcode {
    PW_D3D9_HELLO = 1, PW_D3D9_STOP = 2, PW_D3D9_CREATE9 = 16,
    PW_D3D9_FACTORY_CALL = 17, PW_D3D9_RELEASE = 18, PW_D3D9_DEVICE_CALL = 19
};
/* Target identity is echoed in replies. Newly returned objects belong in a
 * typed payload, never in a pointer field. Sequence is zero for send; replies
 * supply the original ticket. The transport assigns sequence itself. */
struct pw_d3d9_message {
    uint32_t opcode, device, object, generation;
    uint64_t sequence, ticket;
    int32_t result;
    uint32_t payload_bytes;
};
/* Local endpoint state, NOT shared/wire storage. Exactly one caller per
 * endpoint. The peer uses its own view; neither embeds a mapped pointer. */
struct pw_d3d9_channel {
    unsigned char *memory;
    size_t bytes;
    uint32_t epoch, role, offset[2], capacity[2], pending_count;
    uint64_t next_send, next_receive;
    struct pw_d3d9_message pending[PW_D3D9_WIRE_PENDING];
};
size_t pw_d3d9_channel_bytes(uint32_t request_capacity, uint32_t reply_capacity);
int pw_d3d9_channel_init(void *memory, size_t bytes, uint32_t epoch,
                        uint32_t request_capacity, uint32_t reply_capacity);
int pw_d3d9_channel_open(struct pw_d3d9_channel *, void *memory, size_t bytes,
                        uint32_t epoch, enum pw_d3d9_wire_role);
uint32_t pw_d3d9_channel_state(const struct pw_d3d9_channel *);
uint32_t pw_d3d9_channel_error(const struct pw_d3d9_channel *);
/* Only the service can accept the handshake or finish shutdown. The client
 * requests STOPPING then publishes STOP after earlier requests. STOPPED keeps
 * replies readable. Any peer can cancel; first failure is sticky. */
int pw_d3d9_channel_ready(struct pw_d3d9_channel *);
int pw_d3d9_channel_stop(struct pw_d3d9_channel *);
int pw_d3d9_channel_stopped(struct pw_d3d9_channel *);
/* Nonzero 31-bit transport reason; backend HRESULTs belong in replies. */
void pw_d3d9_channel_cancel(struct pw_d3d9_channel *, uint32_t error);
int pw_d3d9_channel_send(struct pw_d3d9_channel *, const struct pw_d3d9_message *, const void *payload);
/* scratch owns a complete copied record on success. Metadata and payload are
 * validated before capacity is released. SMALL/FULL leave the record queued. */
int pw_d3d9_channel_receive(struct pw_d3d9_channel *, struct pw_d3d9_message *,
                            void *scratch, size_t scratch_bytes);
/* Checked byte slices for typed payload decoders: overflow and overlap with
 * a fixed prefix are rejected. Zero length permits only offset zero. */
int pw_d3d9_wire_slice(uint32_t payload_bytes, uint32_t fixed_bytes,
                      uint32_t offset, uint32_t length, uint32_t alignment);
#endif
