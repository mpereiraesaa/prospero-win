/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_VK_REPLAY_H
#define PW_VK_REPLAY_H
#include <stddef.h>
#include <stdint.h>
#define PW_VK_REPLAY_MAX_WORKERS 8
struct pw_vk_replay;
struct pw_vk_replay_lane;
/* Runs only on a host pthread. It must not call guest code or re-enter this
 * scheduler. Zero means success; nonzero makes failure sticky. */
typedef int (*pw_vk_replay_fn)(void *lane_context,const void *owned,size_t bytes);
enum pw_vk_replay_status {
 PW_VK_REPLAY_OK=0, PW_VK_REPLAY_INVALID=-1, PW_VK_REPLAY_MEMORY=-2,
 PW_VK_REPLAY_FAILED=-3, PW_VK_REPLAY_EXHAUSTED=-4
};
struct pw_vk_replay_stats {
 uint64_t submitted,completed,capacity_waits,completion_waits;
 uint64_t worker_jobs[PW_VK_REPLAY_MAX_WORKERS];
 size_t owned_bytes,peak_owned_bytes;
 unsigned active,peak_active,workers;
 int callback_error;
};
/* byte_limit covers queued and executing job allocations. Workers 1..8. */
struct pw_vk_replay *pw_vk_replay_create(unsigned workers,size_t byte_limit,pw_vk_replay_fn);
/* Each lane is one command-buffer generation. Equal nonzero pool identities
 * exclude each other, including across lanes. Context stays alive until drop.
 * Caller serializes lane creation/drop with its own object lifecycle. */
struct pw_vk_replay_lane *pw_vk_replay_lane_create(struct pw_vk_replay *,uint64_t pool,void *context);
int pw_vk_replay_enqueue(struct pw_vk_replay_lane *,const void *,size_t,uint64_t *ticket);
uint64_t pw_vk_replay_marker(struct pw_vk_replay_lane *);
int pw_vk_replay_wait(struct pw_vk_replay_lane *,uint64_t ticket);
/* Pool/global barriers require caller to stop new submissions in that scope. */
int pw_vk_replay_wait_pool(struct pw_vk_replay *,uint64_t pool);
int pw_vk_replay_wait_all(struct pw_vk_replay *);
/* Waits for the lane. On failure retains it until scheduler destruction. */
int pw_vk_replay_lane_drop(struct pw_vk_replay_lane *);
void pw_vk_replay_get_stats(struct pw_vk_replay *,struct pw_vk_replay_stats *);
/* No concurrent API callers. Cancels queued work and joins executing callbacks
 * before freeing lanes; caller context remains live through this call. */
void pw_vk_replay_destroy(struct pw_vk_replay *);
#endif
