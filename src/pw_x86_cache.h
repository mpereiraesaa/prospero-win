/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_X86_CACHE_H
#define PW_X86_CACHE_H
#include "pw_x86_block.h"

typedef struct PwX86LinkSlot {
    void *target_code;           /* host code entry or stub address */
    void *canonical_code;        /* canonical entry of target block when reconciled */
    uint32_t target_pc;          /* guest PC targeted by this exit */
    uint32_t source_pc;          /* guest PC of source block */
    unsigned is_linked;          /* 1 if pointing to a compiled block, 0 if stub */
    unsigned is_reconciled;      /* 1 if linked via reconciliation stub */
} PwX86LinkSlot;

typedef struct PwX86CacheEntry {
    uint32_t guest_pc,generation;
    size_t code_offset,code_bytes,source_bytes;
    size_t canonical_entry_offset;
    size_t chain_entry_offset;
    uint32_t instructions;
    uint16_t instruction_ends[32];
    PwX86RegContract entry_contract;
    PwX86RegContract exit_contract;
    PwX86ExitDesc exit;
    PwX86LinkSlot link_slots[2]; /* 0: target, 1: fallthrough */
    /* Exits still waiting for their target block, chained per bucket of
     * target PC: an exit is (entry index * 2 + side + 1), 0 ends a chain.
     * pending_head belongs to the bucket this slot's index names, not to
     * the block stored here, so publishing into the slot keeps it. */
    uint32_t pending_next[2];
    uint32_t pending_head;
    size_t fault_table_offset;  /* PwX86Block.fault_table_offset */
    size_t exit_offset;         /* PwX86Block.exit_offset */
    uint32_t arena_next;        /* the next block in the arena, as index + 1 */
    unsigned used;
} PwX86CacheEntry;

typedef struct PwX86Cache {
    PwX86CacheEntry *entries;
    uint32_t capacity,generation;
    size_t arena_bytes,cursor;
    uint64_t hits,misses,publishes,resets,lookup_probes;
    uint32_t max_probe;
} PwX86Cache;

/* Generated code storage and RW/RX transitions remain owner-managed. Entries
 * use open addressing keyed by guest PC, making hot dispatch O(1) average
 * rather than scanning the complete capacity. A generation is valid only
 * while its immutable guest image mapping is alive. */
int pw_x86_cache_init(PwX86Cache *,PwX86CacheEntry *,uint32_t,size_t,uint32_t);
int pw_x86_cache_lookup(PwX86Cache *,uint32_t,const PwX86CacheEntry **);
int pw_x86_cache_lookup_mut(PwX86Cache *,uint32_t,PwX86CacheEntry **);
int pw_x86_cache_publish(PwX86Cache *,uint32_t,const PwX86Block *,size_t,
                         const PwX86CacheEntry **);
int pw_x86_cache_reset(PwX86Cache *,uint32_t);
#endif
