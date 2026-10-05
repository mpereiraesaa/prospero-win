/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_x86_cache.h"
#include <string.h>

static uint32_t first_slot(const PwX86Cache *cache,uint32_t guest_pc)
{
    /* The final xor-fold avoids clustering on the aligned low PC bits. */
    uint32_t hash=guest_pc*2654435761u;hash^=hash>>16;
    return hash%cache->capacity;
}

static void record_probes(PwX86Cache *cache,uint32_t probes)
{
    cache->lookup_probes+=probes;
    if(probes>cache->max_probe)cache->max_probe=probes;
}

int pw_x86_cache_init(PwX86Cache *cache,PwX86CacheEntry *entries,uint32_t capacity,
                      size_t arena_bytes,uint32_t generation)
{
    if(!cache || !entries || !capacity || !arena_bytes || !generation)
        return PW_ERR_PRECONDITION;
    memset(entries,0,sizeof(*entries)*capacity);
    *cache=(PwX86Cache){.entries=entries,.capacity=capacity,
                       .generation=generation,.arena_bytes=arena_bytes};
    return PW_OK;
}

int pw_x86_cache_lookup(PwX86Cache *cache,uint32_t guest_pc,const PwX86CacheEntry **entry)
{
    return pw_x86_cache_lookup_mut(cache,guest_pc,(PwX86CacheEntry **)entry);
}

int pw_x86_cache_lookup_mut(PwX86Cache *cache,uint32_t guest_pc,PwX86CacheEntry **entry)
{
    if(!cache || !cache->entries || !cache->generation || !entry)
        return PW_ERR_PRECONDITION;
    uint32_t slot=first_slot(cache,guest_pc);
    for(uint32_t probe=1;probe<=cache->capacity;probe++) {
        PwX86CacheEntry *candidate=&cache->entries[slot];
        if(!candidate->used) {
            record_probes(cache,probe);cache->misses++;*entry=NULL;
            return PW_ERR_NOT_FOUND;
        }
        if(candidate->generation==cache->generation && candidate->guest_pc==guest_pc &&
           !candidate->retired) {
            record_probes(cache,probe);cache->hits++;*entry=candidate;return PW_OK;
        }
        slot=(slot+1)%cache->capacity;
    }
    record_probes(cache,cache->capacity);cache->misses++;*entry=NULL;
    return PW_ERR_NOT_FOUND;
}

int pw_x86_cache_publish(PwX86Cache *cache,uint32_t guest_pc,const PwX86Block *block,
                         size_t code_offset,const PwX86CacheEntry **entry)
{
    if(!cache || !cache->entries || !cache->generation || !block || !entry ||
       !block->source_bytes || !block->code_bytes || !block->instructions)
        return PW_ERR_PRECONDITION;
    if(code_offset!=cache->cursor || code_offset>cache->arena_bytes ||
       block->code_bytes>cache->arena_bytes-code_offset)
        return PW_ERR_LIMIT;
    /* A nearly full table turns every miss into a scan of its clusters. */
    if(cache->occupied>=cache->capacity-cache->capacity/4)return PW_ERR_LIMIT;
    uint32_t slot=first_slot(cache,guest_pc);unsigned available=0;
    for(uint32_t probe=0;probe<cache->capacity;probe++) {
        PwX86CacheEntry *candidate=&cache->entries[slot];
        if(!candidate->used){available=1;break;}
        if(candidate->generation==cache->generation && candidate->guest_pc==guest_pc &&
           !candidate->retired)
            return PW_ERR_STATE;
        slot=(slot+1)%cache->capacity;
    }
    if(!available)return PW_ERR_LIMIT;
    uint32_t pending_head=cache->entries[slot].pending_head;
    cache->entries[slot]=(PwX86CacheEntry){
        .guest_pc=guest_pc,.generation=cache->generation,.code_offset=code_offset,
        .code_bytes=block->code_bytes,.source_bytes=block->source_bytes,
        .canonical_entry_offset=block->canonical_entry_offset,
        .chain_entry_offset=block->chain_entry_offset,
        .instructions=block->instructions,
        .entry_contract=block->entry_contract,
        .exit_contract=block->exit_contract,
        .exit=block->exit,.pending_head=pending_head,
        .fault_table_offset=block->fault_table_offset,.exit_offset=block->exit_offset,
        .source_copy_offset=block->source_copy_offset,.used=1};
    cache->entries[slot].link_slots[0]=(PwX86LinkSlot){
        .target_pc=block->exit.target_pc,.source_pc=guest_pc,.target_code=NULL,.canonical_code=NULL,.is_linked=0,.is_reconciled=0};
    cache->entries[slot].link_slots[1]=(PwX86LinkSlot){
        .target_pc=block->exit.fallthrough_pc,.source_pc=guest_pc,.target_code=NULL,.canonical_code=NULL,.is_linked=0,.is_reconciled=0};
    memcpy(cache->entries[slot].instruction_ends,block->instruction_ends,
           block->instructions*sizeof(block->instruction_ends[0]));
    size_t end=code_offset+block->code_bytes;
    cache->cursor=(end+15)&~(size_t)15;
    if(cache->cursor>cache->arena_bytes)cache->cursor=cache->arena_bytes;
    cache->publishes++;cache->occupied++;*entry=&cache->entries[slot];return PW_OK;
}

int pw_x86_cache_reset(PwX86Cache *cache,uint32_t generation)
{
    if(!cache || !cache->entries || !cache->capacity || !generation ||
       generation==cache->generation)return PW_ERR_PRECONDITION;
    memset(cache->entries,0,sizeof(*cache->entries)*cache->capacity);
    cache->generation=generation;cache->cursor=0;cache->occupied=0;cache->resets++;return PW_OK;
}

int pw_x86_cache_next_retired(PwX86Cache *cache,uint32_t guest_pc,uint32_t *cursor,PwX86CacheEntry **entry)
{
    if(!cache || !cache->entries || !cache->generation || !cursor || !entry)
        return PW_ERR_PRECONDITION;
    const uint32_t first=first_slot(cache,guest_pc);
    for(uint32_t probe=*cursor;probe<cache->capacity;probe++) {
        PwX86CacheEntry *candidate=&cache->entries[(first+probe)%cache->capacity];
        if(!candidate->used)break;
        if(candidate->generation==cache->generation && candidate->guest_pc==guest_pc &&
           candidate->retired) {
            *cursor=probe+1;*entry=candidate;return PW_OK;
        }
    }
    *cursor=cache->capacity;*entry=NULL;
    return PW_ERR_NOT_FOUND;
}
