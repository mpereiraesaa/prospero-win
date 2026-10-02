/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_x86_engine.h"
#include "pw_x86_reencode.h"
#include "pw_guest_fp.h"
#include <string.h>

#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
static int invoke(void *entry,PwX86State *state)
{
    return pw_x86_run_block(state,entry);
}

static int protection(PwX86Engine *engine,size_t offset,size_t bytes,unsigned value)
{
    int status=engine->backend->protect(engine->backend->context,&engine->code,
                                        offset,bytes,value);
    if(status==PW_OK) {
        engine->protection_calls++;engine->protection_bytes+=bytes;
        engine->sealed=value==(PW_PROT_READ|PW_PROT_EXEC);
    }
    return status;
}

/* A re-encoded exit jumps straight to its slot's target_code through a
 * rel32 (PwX86ExitDesc.*_direct_offset): rewrite it whenever target_code
 * changes. */
static void sync_direct(PwX86Engine *engine, const PwX86CacheEntry *entry, unsigned side)
{
    const size_t at = side ? entry->exit.fallthrough_direct_offset : entry->exit.target_direct_offset;
    if(!at || !entry->link_slots[side].target_code) return;
    const size_t offset = entry->code_offset + at;
    const uint8_t *next = (const uint8_t *)engine->code.exec_base + offset + 4;
    const int32_t rel = (int32_t)((const uint8_t *)entry->link_slots[side].target_code - next);
    const size_t page = engine->backend->page_bytes, first = (offset / page) * page;
    const size_t end = ((offset + 4 + page - 1) / page) * page;
    const unsigned was_sealed = engine->sealed;

    if(was_sealed && protection(engine, first, end - first, PW_PROT_READ|PW_PROT_WRITE) != PW_OK) {
        engine->failed = 1; return;
    }
    memcpy((uint8_t *)engine->code.write_base + offset, &rel, sizeof(rel));
    if(was_sealed && protection(engine, first, end - first, PW_PROT_READ|PW_PROT_EXEC) != PW_OK)
        engine->failed = 1;
}

/* The bucket an exit waits in until its target PC is compiled: the target's
 * home slot in the cache's hash. */
static uint32_t target_bucket(const PwX86Cache *cache,uint32_t target_pc)
{
    uint32_t hash=target_pc*2654435761u;hash^=hash>>16;
    return hash%cache->capacity;
}

static uint32_t *pending_bucket(PwX86Cache *cache,uint32_t pc)
{
    uint32_t bucket=target_bucket(cache,pc);
    pw_x86_cache_touch(cache,bucket);
    return &cache->entries[bucket].pending_head;
}

static void wait_for_target(PwX86Cache *cache,PwX86CacheEntry *entry,unsigned side)
{
    if(entry->link_slots[side].is_linked)return;
    uint32_t *head=pending_bucket(cache,entry->link_slots[side].target_pc);
    entry->pending_next[side]=*head;
    *head=(uint32_t)(entry-cache->entries)*2+side+1;
}

int pw_x86_engine_init(PwX86Engine *engine,const PwVmBackend *backend,
                       PwX86CacheEntry *entries,uint32_t capacity,size_t arena_bytes,
                       uint32_t generation,PwX86SourceView source_view,void *opaque)
{
    if(!engine || !backend || !entries || !capacity || !arena_bytes || !generation ||
       !source_view || !pw_vm_backend_valid(backend) ||
       !(backend->capabilities&PW_VM_CAP_PROTECT))return PW_ERR_PRECONDITION;
    memset(engine,0,sizeof(*engine));
    int status=backend->reserve(backend->context,arena_bytes,backend->page_bytes,&engine->code);
    if(status!=PW_OK)return status;
    status=backend->commit(backend->context,&engine->code,0,engine->code.bytes,
                           PW_PROT_READ|PW_PROT_WRITE);
    if(status!=PW_OK){(void)backend->release(backend->context,&engine->code);return status;}
    status=pw_x86_cache_init(&engine->cache,entries,capacity,engine->code.bytes,generation);
    if(status!=PW_OK){(void)backend->release(backend->context,&engine->code);return status;}
    engine->backend=backend;engine->source_view=source_view;engine->source_opaque=opaque;
    engine->quantum=PW_X86_ENGINE_DEFAULT_QUANTUM;
    engine->chaining_enabled=0;
    engine->residency_enabled=1;
    engine->lazy_flags_enabled=1;
    engine->initialized=1;return PW_OK;
}

int pw_x86_engine_set_quantum(PwX86Engine *engine, uint32_t quantum)
{
    if(!engine || !engine->initialized) return PW_ERR_PRECONDITION;
    engine->quantum = quantum;
    return PW_OK;
}

int pw_x86_engine_set_chaining(PwX86Engine *engine, unsigned enabled)
{
    if(!engine || !engine->initialized) return PW_ERR_PRECONDITION;
    engine->chaining_enabled = enabled;
    return PW_OK;
}

int pw_x86_engine_set_residency(PwX86Engine *engine, unsigned enabled)
{
    if(!engine || !engine->initialized) return PW_ERR_PRECONDITION;
    engine->residency_enabled = enabled ? 1 : 0;
    return PW_OK;
}

int pw_x86_engine_set_lazy_flags(PwX86Engine *engine, unsigned enabled)
{
    if(!engine || !engine->initialized) return PW_ERR_PRECONDITION;
    engine->lazy_flags_enabled = enabled ? 1 : 0;
    return PW_OK;
}

int pw_x86_engine_set_global_resident(PwX86Engine *engine, uint8_t mask)
{
    unsigned count = 0;

    if(!engine || !engine->initialized) return PW_ERR_PRECONDITION;
    for(unsigned g = 0; g < 8; g++) count += (mask >> g) & 1u;
    if(count > 7) return PW_ERR_PRECONDITION;
    engine->global_resident = mask;
    return PW_OK;
}

int pw_x86_engine_set_reencode(PwX86Engine *engine, unsigned enabled)
{
    if(!engine || !engine->initialized) return PW_ERR_PRECONDITION;
    if(enabled && !engine->chain_targets) {
        const size_t bytes=PW_X86_REENCODE_CHAIN_ENTRIES*sizeof(PwX86IndirectTarget);
        int status=engine->backend->reserve(engine->backend->context,bytes,
                                            engine->backend->page_bytes,&engine->chain);
        if(status!=PW_OK)return status;
        status=engine->backend->commit(engine->backend->context,&engine->chain,0,
                                       engine->chain.bytes,PW_PROT_READ|PW_PROT_WRITE);
        /* Wine's lazy backend makes pages accessible through protect;
         * the whole-region commit call above only validates the range. */
        if(status==PW_OK) status=engine->backend->protect(engine->backend->context,&engine->chain,
                                                        0,engine->chain.bytes,PW_PROT_READ|PW_PROT_WRITE);
        if(status!=PW_OK) {
            (void)engine->backend->release(engine->backend->context,&engine->chain);
            return status;
        }
        engine->chain_targets=engine->chain.write_base;
        memset(engine->chain_targets,0,bytes);
    }
    engine->reencode_enabled = enabled ? 1 : 0;
    return PW_OK;
}

int pw_x86_engine_set_fault_markers(PwX86Engine *engine, unsigned enabled)
{
    if(!engine || !engine->initialized) return PW_ERR_PRECONDITION;
    if(enabled && !engine->block_map) {
        const size_t bytes=(engine->code.bytes/PW_X86_ENGINE_FAULT_GRANULE+1)*sizeof(uint32_t);
        int status=engine->backend->reserve(engine->backend->context,bytes,
                                            engine->backend->page_bytes,&engine->block_map_region);
        if(status!=PW_OK)return status;
        status=engine->backend->commit(engine->backend->context,&engine->block_map_region,0,
                                       engine->block_map_region.bytes,PW_PROT_READ|PW_PROT_WRITE);
        if(status!=PW_OK) {
            (void)engine->backend->release(engine->backend->context,&engine->block_map_region);
            return status;
        }
        engine->block_map=engine->block_map_region.write_base;
        memset(engine->block_map,0,bytes);
        engine->last_published=0;
    }
    engine->fault_markers = enabled ? 1 : 0;
    return PW_OK;
}

/* The return stub at the start of the arena (pw_x86_reencode_return_stub);
 * blocks follow it. */
static int emit_return_stub(PwX86Engine *engine)
{
    uint8_t stub[512];
    PwX86TranslateOptions options;
    size_t bytes;

    if(!engine->call_stack_base) return PW_OK;
    memset(&options, 0, sizeof(options));
    options.indirect_targets = engine->indirect_targets;
    options.indirect_mask = PW_X86_ENGINE_INDIRECT_SLOTS - 1;
    options.chain_targets = engine->chain_targets;
    options.unbounded_chains = 1;
    options.call_stack = 1;
    options.native_fp = engine->native_fp;
    if(!(bytes = pw_x86_reencode_return_stub(stub, sizeof(stub), &options))) return PW_ERR_UNSUPPORTED;
    const size_t page = engine->backend->page_bytes, end = ((bytes + page - 1) / page) * page;
    if(protection(engine, 0, end, PW_PROT_READ|PW_PROT_WRITE) != PW_OK) return PW_ERR_VM;
    memcpy(engine->code.write_base, stub, bytes);
    if(protection(engine, 0, end, PW_PROT_READ|PW_PROT_EXEC) != PW_OK) return PW_ERR_VM;
    engine->return_stub_bytes = (bytes + 63) & ~(size_t)63;
    engine->cache.cursor = engine->return_stub_bytes;
    return PW_OK;
}

int pw_x86_engine_set_call_stack(PwX86Engine *engine, void *base, size_t bytes)
{
    if(!engine || !engine->initialized) return PW_ERR_PRECONDITION;
    if(!base) {
        engine->call_stack_base = NULL;
        engine->call_stack_bytes = 0;
        engine->call_stack_top = 0;
        return PW_OK;
    }
    if(bytes < 4096 || !engine->reencode_enabled || !engine->indirect_targets || !engine->chain_targets ||
       !engine->unbounded_chains || engine->cache.cursor)
        return PW_ERR_PRECONDITION;
    engine->call_stack_base = base;
    engine->call_stack_bytes = bytes;
    /* A few return addresses above the top all lead to the return stub. */
    uintptr_t top = ((uintptr_t)base + bytes - 16 * sizeof(uintptr_t)) & ~(uintptr_t)15;
    for(uintptr_t *slot = (uintptr_t *)top; (uintptr_t)(slot + 1) <= (uintptr_t)base + bytes; slot++)
        *slot = (uintptr_t)engine->code.exec_base;
    engine->call_stack_top = top;
    int status = emit_return_stub(engine);
    if(status != PW_OK) {
        engine->call_stack_base = NULL;
        engine->call_stack_top = 0;
    }
    return status;
}

int pw_x86_engine_call_stack_fault(const PwX86Engine *engine, uintptr_t address, uintptr_t *rsp)
{
    const uintptr_t base = (uintptr_t)engine->call_stack_base;
    if(!base || address >= base || address < base - PW_X86_ENGINE_CALL_STACK_GUARD) return 0;
    *rsp = engine->call_stack_top;
    return 1;
}

static uint8_t *fp_image(PwX86Engine *engine)
{
    return (uint8_t *)(((uintptr_t)engine->fxsave_image + 15) & ~(uintptr_t)15);
}

void pw_x86_engine_fp_sync(PwX86Engine *engine, PwX86State *state)
{
    if (!engine || !state || !engine->fp_image_live) return;
    pw_guest_fp_from_fxsave(&state->fp, fp_image(engine));
    engine->fp_image_live = 0;
}

void pw_x86_engine_fp_load(PwX86Engine *engine, PwX86State *state, const uint8_t image[512])
{
    if (!engine || !state || !image) return;
    if (!engine->native_fp) {
        pw_guest_fp_from_fxsave(&state->fp, image);
        return;
    }
    /* What a conversion keeps beyond the image: the pending-exception bits
     * stay in state->fp, and the state is initialised. */
    memcpy(fp_image(engine), image, 512);
    state->fp.initialized = 1;
    engine->fp_image_live = 1;
}

void pw_x86_engine_fp_store(PwX86Engine *engine, const PwX86State *state, uint8_t image[512])
{
    if (!engine || !state || !image) return;
    if (engine->fp_image_live) memcpy(image, fp_image(engine), 512);
    else pw_guest_fp_to_fxsave(&state->fp, image);
}

int pw_x86_engine_set_native_fp(PwX86Engine *engine, unsigned enabled)
{
    if(!engine || !engine->initialized) return PW_ERR_PRECONDITION;
    engine->native_fp = enabled ? 1 : 0;
    return PW_OK;
}

/* With native FP, re-encoded and emitted blocks meet only through C, which
 * moves the guest's FP state between the host FPU and memory. */
static int may_link(const PwX86Engine *engine, const PwX86RegContract *from, const PwX86RegContract *to)
{
    return !engine->native_fp || pw_x86_reencoded(from) == pw_x86_reencoded(to);
}

int pw_x86_engine_set_superblocks(PwX86Engine *engine, unsigned enabled)
{
    if(!engine || !engine->initialized) return PW_ERR_PRECONDITION;
    engine->superblocks = enabled ? 1 : 0;
    return PW_OK;
}

int pw_x86_engine_set_unbounded_chains(PwX86Engine *engine, unsigned enabled)
{
    if(!engine || !engine->initialized) return PW_ERR_PRECONDITION;
    engine->unbounded_chains = enabled ? 1 : 0;
    return PW_OK;
}

/* Record a published block for pw_x86_engine_fault_redirect. */
static void map_block(PwX86Engine *engine, const PwX86CacheEntry *entry)
{
    const uint32_t index=(uint32_t)(entry-engine->cache.entries)+1;
    if(!engine->block_map || !entry->code_bytes) return;
    if(engine->last_published) engine->cache.entries[engine->last_published-1].arena_next=index;
    engine->last_published=index;
    for(size_t g=entry->code_offset/PW_X86_ENGINE_FAULT_GRANULE;
        g<=(entry->code_offset+entry->code_bytes-1)/PW_X86_ENGINE_FAULT_GRANULE; g++)
        if(!engine->block_map[g]) engine->block_map[g]=index;
}

const PwX86CacheEntry *pw_x86_engine_host_block(const PwX86Engine *engine, uintptr_t rip)
{
    const uintptr_t low = (uintptr_t)engine->code.exec_base;
    uint32_t index;

    if(!engine->block_map || rip < low || rip - low >= engine->code.bytes) return NULL;
    index = engine->block_map[(rip - low) / PW_X86_ENGINE_FAULT_GRANULE];
    while(index && index <= engine->cache.capacity) {
        const PwX86CacheEntry *e = &engine->cache.entries[index - 1];
        const uintptr_t start = low + e->code_offset;
        if(!e->used || e->generation != engine->cache.generation || rip < start) return NULL;
        if(rip - start < e->code_bytes) return e;
        index = e->arena_next;
    }
    return NULL;
}

void pw_x86_engine_sample(const PwX86Engine *engine, uintptr_t rip, PwX86HotspotProfile *profile)
{
    const uintptr_t base = (uintptr_t)engine->code.exec_base;
    const PwX86CacheEntry *entry;
    unsigned bucket;

    profile->samples++;
    if(rip < base || rip - base >= engine->code.bytes) { profile->outside++; return; }
    entry = pw_x86_engine_host_block(engine, rip);
    if(!entry) { profile->stubs++; return; }
    bucket = (entry->guest_pc * 2654435761u) & (PW_X86_HOTSPOT_SLOTS - 1);
    for(unsigned probe = 0; probe < PW_X86_HOTSPOT_SLOTS; probe++) {
        PwX86Hotspot *row = &profile->slots[bucket];
        if(!row->samples || row->guest_pc == entry->guest_pc) {
            const size_t offset = rip - base - entry->code_offset;
            row->guest_pc = entry->guest_pc;
            row->samples++;
            if(!pw_x86_reencoded(&entry->entry_contract) || !entry->exit_offset) row->emitted++;
            else if(offset < entry->chain_entry_offset) row->entry++;
            else if(offset < entry->exit_offset) row->body++;
            else row->exit++;
            return;
        }
        bucket = (bucket + 1) & (PW_X86_HOTSPOT_SLOTS - 1);
    }
    profile->overflow++;
}

uintptr_t pw_x86_engine_fault_redirect(const PwX86Engine *engine, uintptr_t rip)
{
    const PwX86CacheEntry *e = pw_x86_engine_host_block(engine, rip);
    uintptr_t start;
    size_t path;

    if(!e || !e->fault_table_offset) return 0;
    start = (uintptr_t)engine->code.exec_base + e->code_offset;
    path = pw_x86_fault_table_path((const uint8_t *)start, e->fault_table_offset, rip - start);
    return path ? start + path : 0;
}

int pw_x86_engine_set_counters(PwX86Engine *engine, unsigned enabled)
{
    if(!engine || !engine->initialized) return PW_ERR_PRECONDITION;
    engine->no_counters = enabled ? 0 : 1;
    return PW_OK;
}

int pw_x86_engine_set_flat_memory(PwX86Engine *engine, uint32_t low, uint32_t high)
{
    if(!engine || !engine->initialized) return PW_ERR_PRECONDITION;
    if(low != high && (high < low || high - low < 16)) return PW_ERR_PRECONDITION;
    engine->flat_low = low == high ? 0 : low;
    engine->flat_high = low == high ? 0 : high;
    return PW_OK;
}

int pw_x86_engine_set_indirect(PwX86Engine *engine, unsigned enabled)
{
    if(!engine || !engine->initialized) return PW_ERR_PRECONDITION;
    if(enabled && !engine->indirect_targets) {
        const size_t bytes=PW_X86_ENGINE_INDIRECT_SLOTS*sizeof(PwX86IndirectTarget);
        int status=engine->backend->reserve(engine->backend->context,bytes,
                                            engine->backend->page_bytes,&engine->indirect);
        if(status!=PW_OK)return status;
        status=engine->backend->commit(engine->backend->context,&engine->indirect,0,
                                       engine->indirect.bytes,PW_PROT_READ|PW_PROT_WRITE);
        if(status!=PW_OK) {
            (void)engine->backend->release(engine->backend->context,&engine->indirect);
            return status;
        }
        engine->indirect_targets=engine->indirect.write_base;
        memset(engine->indirect_targets,0,bytes);
    }
    engine->indirect_enabled = enabled ? 1 : 0;
    return PW_OK;
}

static int compile(PwX86Engine *engine,uint32_t pc,const PwX86CacheEntry **entry)
{
    const uint8_t *source=NULL;size_t available=0;
    int status=engine->source_view(engine->source_opaque,pc,&source,&available);
    if(status!=PW_OK)return status;
    if(!source || !available)return PW_ERR_NOT_FOUND;
    if(available>PW_X86_ENGINE_MAX_SOURCE)available=PW_X86_ENGINE_MAX_SOURCE;
    uint8_t scratch[PW_X86_ENGINE_MAX_CODE];
    PwX86Block best = {0};
    const PwX86TranslateOptions options = {
        engine->residency_enabled, engine->lazy_flags_enabled,
        engine->indirect_enabled ? engine->indirect_targets : NULL,
        PW_X86_ENGINE_INDIRECT_SLOTS - 1, engine->flat_low, engine->flat_high,
        engine->no_counters,
        engine->reencode_enabled ? engine->chain_targets : NULL,
        engine->residency_enabled ? engine->global_resident : (uint8_t)0,
        engine->fault_markers, engine->unbounded_chains, engine->call_stack_base != NULL,
        engine->superblocks, engine->native_fp };
    int last = PW_ERR_UNSUPPORTED;
    if (engine->reencode_enabled) {
        last = pw_x86_reencode(source, available, pc, scratch, sizeof(scratch), &best, &options);
        if (last == PW_OK) engine->reencoded_blocks++;
    }
    if (last != PW_OK)
        last = pw_x86_translate_opts(source, available, pc, scratch, sizeof(scratch), &best, &options);
    if (last != PW_OK) return last;
    if (!best.instructions) return PW_ERR_TRUNCATED;
    if(best.code_bytes>engine->cache.arena_bytes-engine->cache.cursor)return PW_ERR_LIMIT;

    /* If chaining is enabled and exit is chainable, patch link slot addresses in scratch */
    if(best.exit.chainable && engine->chaining_enabled) {
        uint32_t hash=pc*2654435761u;hash^=hash>>16;
        uint32_t slot=hash%engine->cache.capacity;
        PwX86CacheEntry *cand = NULL;
        for(uint32_t probe=0; probe<engine->cache.capacity; probe++) {
            PwX86CacheEntry *c=&engine->cache.entries[slot];
            if(!c->used || (c->generation==engine->cache.generation && c->guest_pc==pc)) {
                cand = c;
                break;
            }
            slot=(slot+1)%engine->cache.capacity;
        }
        if(cand) {
            uintptr_t taken_slot = (uintptr_t)&cand->link_slots[0].target_code;
            memcpy(scratch + best.exit.target_patch_offset, &taken_slot, sizeof(taken_slot));
            if(best.exit.target_reconcile_patch_offset) {
                uintptr_t taken_canonical = (uintptr_t)&cand->link_slots[0].canonical_code;
                memcpy(scratch + best.exit.target_reconcile_patch_offset, &taken_canonical, sizeof(taken_canonical));
            }
            if(best.exit.kind == PW_X86_EXIT_CONDITIONAL) {
                uintptr_t fallthrough_slot = (uintptr_t)&cand->link_slots[1].target_code;
                memcpy(scratch + best.exit.fallthrough_patch_offset, &fallthrough_slot, sizeof(fallthrough_slot));
                if(best.exit.fallthrough_reconcile_patch_offset) {
                    uintptr_t fallthrough_canonical = (uintptr_t)&cand->link_slots[1].canonical_code;
                    memcpy(scratch + best.exit.fallthrough_reconcile_patch_offset, &fallthrough_canonical, sizeof(fallthrough_canonical));
                }
            }
        }
    }

    size_t page=engine->backend->page_bytes;
    size_t first=(engine->cache.cursor/page)*page;
    size_t tail=engine->cache.cursor+best.code_bytes;
    size_t end=((tail+page-1)/page)*page;
    if(end>engine->code.bytes)end=engine->code.bytes;
    if(protection(engine,first,end-first,PW_PROT_READ|PW_PROT_WRITE)!=PW_OK)
        return PW_ERR_VM;
    memcpy((uint8_t *)engine->code.write_base+engine->cache.cursor,scratch,best.code_bytes);
    status=pw_x86_cache_publish(&engine->cache,pc,&best,engine->cache.cursor,entry);
    if(status!=PW_OK) {
        if(protection(engine,first,end-first,PW_PROT_READ|PW_PROT_EXEC)!=PW_OK)
            engine->failed=1;
        return status;
    }
    if(protection(engine,first,end-first,PW_PROT_READ|PW_PROT_EXEC)!=PW_OK) {
        engine->failed=1;return PW_ERR_VM;
    }

    /* Initialize link slot stubs */
    PwX86CacheEntry *e_mut = (PwX86CacheEntry *)*entry;
    map_block(engine, e_mut);
    uint8_t *exec_base = (uint8_t *)engine->code.exec_base;
    if(best.exit.chainable) {
        e_mut->link_slots[0].target_code = exec_base + e_mut->code_offset + best.exit.target_stub_offset;
        e_mut->link_slots[0].canonical_code = NULL;
        e_mut->link_slots[0].target_pc = best.exit.target_pc;
        e_mut->link_slots[0].source_pc = pc;
        e_mut->link_slots[0].is_linked = 0;
        e_mut->link_slots[0].is_reconciled = 0;
        if(best.exit.kind == PW_X86_EXIT_CONDITIONAL) {
            e_mut->link_slots[1].target_code = exec_base + e_mut->code_offset + best.exit.fallthrough_stub_offset;
            e_mut->link_slots[1].canonical_code = NULL;
            e_mut->link_slots[1].target_pc = best.exit.fallthrough_pc;
            e_mut->link_slots[1].source_pc = pc;
            e_mut->link_slots[1].is_linked = 0;
            e_mut->link_slots[1].is_reconciled = 0;
        }

        /* Forward link: connect newly published exits to targets already in cache */
        if(engine->chaining_enabled) {
            PwX86CacheEntry *tgt = NULL;
            if(pw_x86_cache_lookup_mut(&engine->cache, best.exit.target_pc, &tgt) == PW_OK &&
               may_link(engine, &e_mut->exit_contract, &tgt->entry_contract)) {
                engine->attempted_links++;
                if(pw_x86_contracts_match(&e_mut->exit_contract, &tgt->entry_contract)) {
                    e_mut->link_slots[0].target_code = exec_base + tgt->code_offset + tgt->chain_entry_offset;
                    e_mut->link_slots[0].canonical_code = exec_base + tgt->code_offset + tgt->canonical_entry_offset;
                    e_mut->link_slots[0].is_reconciled = 0;
                } else {
                    e_mut->link_slots[0].target_code = exec_base + e_mut->code_offset + best.exit.target_reconcile_offset;
                    e_mut->link_slots[0].canonical_code = exec_base + tgt->code_offset + tgt->canonical_entry_offset;
                    e_mut->link_slots[0].is_reconciled = 1;
                }
                e_mut->link_slots[0].is_linked = 1;
                sync_direct(engine, e_mut, 0);
                engine->successful_links++;
            }
            if(best.exit.kind == PW_X86_EXIT_CONDITIONAL) {
                if(pw_x86_cache_lookup_mut(&engine->cache, best.exit.fallthrough_pc, &tgt) == PW_OK &&
                   may_link(engine, &e_mut->exit_contract, &tgt->entry_contract)) {
                    engine->attempted_links++;
                    if(pw_x86_contracts_match(&e_mut->exit_contract, &tgt->entry_contract)) {
                        e_mut->link_slots[1].target_code = exec_base + tgt->code_offset + tgt->chain_entry_offset;
                        e_mut->link_slots[1].canonical_code = exec_base + tgt->code_offset + tgt->canonical_entry_offset;
                        e_mut->link_slots[1].is_reconciled = 0;
                    } else {
                        e_mut->link_slots[1].target_code = exec_base + e_mut->code_offset + best.exit.fallthrough_reconcile_offset;
                        e_mut->link_slots[1].canonical_code = exec_base + tgt->code_offset + tgt->canonical_entry_offset;
                        e_mut->link_slots[1].is_reconciled = 1;
                    }
                    e_mut->link_slots[1].is_linked = 1;
                    sync_direct(engine, e_mut, 1);
                    engine->successful_links++;
                }
            }
        }
    }

    /* Backward link: connect existing unlinked exits targeting this PC. Only
     * the exits waiting in this PC's bucket are visited; scanning the whole
     * cache on every compile made translation cost grow with cache size. */
    if(engine->chaining_enabled && best.exit.chainable) {
        wait_for_target(&engine->cache, e_mut, 0);
        if(best.exit.kind == PW_X86_EXIT_CONDITIONAL) wait_for_target(&engine->cache, e_mut, 1);
    }
    if(engine->chaining_enabled) {
        uint32_t *link = pending_bucket(&engine->cache,pc);
        while(*link) {
            unsigned side = (*link - 1) & 1;
            PwX86CacheEntry *cand = &engine->cache.entries[(*link - 1) >> 1];
            PwX86LinkSlot *slot = &cand->link_slots[side];
            uint32_t *next = &cand->pending_next[side];

            if(!slot->is_linked && slot->target_pc != pc) { link = next; continue; }
            if(!slot->is_linked && may_link(engine, &cand->exit_contract, &e_mut->entry_contract)) {
                engine->attempted_links++;
                if(pw_x86_contracts_match(&cand->exit_contract, &e_mut->entry_contract)) {
                    slot->target_code = exec_base + e_mut->code_offset + e_mut->chain_entry_offset;
                    slot->canonical_code = exec_base + e_mut->code_offset + e_mut->canonical_entry_offset;
                    slot->is_reconciled = 0;
                } else {
                    slot->target_code = exec_base + cand->code_offset +
                        (side ? cand->exit.fallthrough_reconcile_offset : cand->exit.target_reconcile_offset);
                    slot->canonical_code = exec_base + e_mut->code_offset + e_mut->canonical_entry_offset;
                    slot->is_reconciled = 1;
                }
                slot->is_linked = 1;
                sync_direct(engine, cand, side);
                engine->successful_links++;
            }
            /* Linked now, or earlier on the dispatch path: stop waiting. */
            *link = *next;
            *next = 0;
        }
    }

    engine->compiles++;return PW_OK;
}

int pw_x86_engine_step(PwX86Engine *engine,PwX86State *state,PwX86StepReport *report)
{
    if(!engine || !state || !report || !engine->initialized)return PW_ERR_PRECONDITION;
    memset(report,0,sizeof(*report));report->guest_pc=state->eip;
    if(engine->dispatch_profile && engine->chain_targets) {
        const PwX86IndirectTarget *target=&engine->chain_targets[pw_x86_chain_slot(state->eip)];
        const PwX86IndirectTarget *second=target+PW_X86_REENCODE_CHAIN_SLOTS;
        if((target->host_code && target->guest_pc==state->eip) ||
           (second->host_code && second->guest_pc==state->eip)) engine->dispatch_chain_matches++;
        else if(!target->host_code && !second->host_code) engine->dispatch_chain_empty++;
        else engine->dispatch_chain_collisions++;
    }

    /* Dynamic unlinked chain resolution: if previous step exited via an unlinked slot, link it now */
    if(engine->chaining_enabled && state->last_exit_slot) {
        PwX86LinkSlot *last_slot = (PwX86LinkSlot *)state->last_exit_slot;
        if(last_slot->target_pc == state->eip && !last_slot->is_linked) {
            PwX86CacheEntry *target_entry = NULL;
            if(pw_x86_cache_lookup_mut(&engine->cache, state->eip, &target_entry) == PW_OK) {
                PwX86CacheEntry *source_entry = NULL;
                if(engine->native_fp &&
                   (pw_x86_cache_lookup_mut(&engine->cache, last_slot->source_pc, &source_entry) != PW_OK ||
                    !may_link(engine, &source_entry->exit_contract, &target_entry->entry_contract)))
                    goto dispatch;
                source_entry = NULL;
                size_t reconcile_offset = 0;
                if(pw_x86_cache_lookup_mut(&engine->cache, last_slot->source_pc, &source_entry) == PW_OK) {
                    if(last_slot == &source_entry->link_slots[0]) {
                        reconcile_offset = source_entry->exit.target_reconcile_offset;
                    } else if(last_slot == &source_entry->link_slots[1]) {
                        reconcile_offset = source_entry->exit.fallthrough_reconcile_offset;
                    }
                }
                engine->attempted_links++;
                if(source_entry && pw_x86_contracts_match(&source_entry->exit_contract, &target_entry->entry_contract)) {
                    last_slot->target_code = (uint8_t *)engine->code.exec_base + target_entry->code_offset + target_entry->chain_entry_offset;
                    last_slot->canonical_code = (uint8_t *)engine->code.exec_base + target_entry->code_offset + target_entry->canonical_entry_offset;
                    last_slot->is_reconciled = 0;
                } else if(source_entry) {
                    last_slot->target_code = (uint8_t *)engine->code.exec_base + source_entry->code_offset + reconcile_offset;
                    last_slot->canonical_code = (uint8_t *)engine->code.exec_base + target_entry->code_offset + target_entry->canonical_entry_offset;
                    last_slot->is_reconciled = 1;
                } else {
                    last_slot->target_code = (uint8_t *)engine->code.exec_base + target_entry->code_offset + target_entry->canonical_entry_offset;
                    last_slot->canonical_code = (uint8_t *)engine->code.exec_base + target_entry->code_offset + target_entry->canonical_entry_offset;
                    last_slot->is_reconciled = 0;
                }
                last_slot->is_linked = 1;
                if(source_entry && (last_slot == &source_entry->link_slots[0] ||
                                    last_slot == &source_entry->link_slots[1]))
                    sync_direct(engine, source_entry, last_slot == &source_entry->link_slots[1]);
                engine->successful_links++;
            }
        }
    }

dispatch:;
    const PwX86CacheEntry *entry=NULL;
    int status=pw_x86_cache_lookup(&engine->cache,state->eip,&entry);
    if(status==PW_OK)report->cache_hit=1;
    else if(status==PW_ERR_NOT_FOUND) {
        status=compile(engine,state->eip,&entry);
        if(status!=PW_OK)return status;
    } else return status;

    report->instructions=entry->instructions;report->source_bytes=entry->source_bytes;
    report->code_bytes=entry->code_bytes;engine->dispatches++;
    if(engine->indirect_enabled) {
        /* The indirect table enters any block at its canonical entry, from
         * emitted code; with native FP, only emitted blocks go there. */
        if(!(engine->native_fp && pw_x86_reencoded(&entry->entry_contract))) {
            PwX86IndirectTarget *target=&engine->indirect_targets[
                pw_x86_indirect_slot(entry->guest_pc,PW_X86_ENGINE_INDIRECT_SLOTS-1)];
            target->guest_pc=entry->guest_pc;
            target->host_code=(const uint8_t *)engine->code.exec_base+entry->code_offset+
                              entry->canonical_entry_offset;
        }
        if(engine->chain_targets && pw_x86_reencoded(&entry->entry_contract)) {
            PwX86IndirectTarget *chain=&engine->chain_targets[pw_x86_chain_slot(entry->guest_pc)];
            PwX86IndirectTarget *second=chain+PW_X86_REENCODE_CHAIN_SLOTS;
            /* Existing targets keep their bank. A new colliding PC displaces
             * the primary into the secondary; no generated-code writes. */
            if(second->host_code && second->guest_pc==entry->guest_pc) chain=second;
            else if(chain->host_code && chain->guest_pc!=entry->guest_pc) *second=*chain;
            chain->guest_pc=entry->guest_pc;
            chain->host_code=(const uint8_t *)engine->code.exec_base+entry->code_offset+
                             entry->chain_entry_offset;
        }
    }

    state->chain_budget = (engine->chaining_enabled && engine->quantum) ? engine->quantum : 1;
    state->step_retired = 0;
    state->step_transitions = 0;
    state->last_exit_slot = 0;
    state->reg_loads = 0;
    state->reg_stores = 0;
    state->reg_reconciliations = 0;
    state->reg_spills = 0;

    state->call_stack_top = engine->call_stack_top;
    void *code_entry=(uint8_t *)engine->code.exec_base+entry->code_offset+entry->canonical_entry_offset;
    int invoked;
    uint64_t execution_begin=0;
    unsigned timed=0;
    if(engine->execution_clock) {
        engine->execution_calls++;
        engine->execution_random=engine->execution_random*1664525u+1013904223u;
        timed=engine->execution_stride==1 ||
              engine->execution_random<=UINT32_MAX/engine->execution_stride;
    }
    if(engine->native_fp && pw_x86_reencoded(&entry->entry_contract)) {
        /* The guest's x87, MMX and SSE state in the host FPU for the chain;
         * it stays in the image afterwards (pw_x86_engine_fp_sync). */
        uint8_t *image=fp_image(engine);
        if(!engine->fp_image_live) {
            pw_guest_fp_to_fxsave(&state->fp,image);
            engine->fp_image_live=1;
        }
        if(timed)
            execution_begin=engine->execution_clock(engine->execution_clock_opaque);
        invoked=pw_x86_run_block_fp(state,code_entry,image);
    } else {
        /* Emitter blocks work on state->fp. */
        pw_x86_engine_fp_sync(engine,state);
        if(timed)
            execution_begin=engine->execution_clock(engine->execution_clock_opaque);
        invoked=invoke(code_entry,state);
    }
    if(timed) {
        uint64_t end=engine->execution_clock(engine->execution_clock_opaque);
        engine->execution_samples++;
        if(!execution_begin || !end || end<execution_begin)engine->execution_clock_errors++;
        else engine->execution_ns+=end-execution_begin;
    }

    /* The block a failed step entered, for the fault report. Taken only on
     * failure: copying it before every dispatch cost a kilobyte per step. */
    if(invoked) {
        engine->fault_block_pc = entry->guest_pc;
        engine->fault_block_instructions = entry->instructions;
        engine->fault_block_resident_mask = entry->entry_contract.resident_mask;
        memcpy(engine->fault_block_map, entry->entry_contract.guest_to_host,
               sizeof(engine->fault_block_map));
        engine->fault_block_code_bytes = entry->code_bytes < sizeof(engine->fault_block_code)
            ? (uint32_t)entry->code_bytes : (uint32_t)sizeof(engine->fault_block_code);
        memcpy(engine->fault_block_code,
               (const uint8_t *)engine->code.exec_base + entry->code_offset,
               engine->fault_block_code_bytes);
    }

    engine->linked_transitions += state->step_transitions;
    engine->reg_loads += state->reg_loads;
    engine->reg_stores += state->reg_stores;
    engine->reg_reconciliations += state->reg_reconciliations;
    engine->reg_spills += state->reg_spills;
    if(state->deferred_flags.known_mask)
        engine->flags_safepoint_commits++;
    pw_x86_commit_canonical_flags(state);

    if(!invoked) {
        report->retired=state->step_retired;
        if(state->chain_budget == 0) engine->safepoint_returns++;
        else engine->dispatcher_transitions++;
    } else {
        uint32_t intra = 0;
        if(state->eip>=entry->guest_pc &&
           (uint64_t)state->eip<=entry->guest_pc+entry->source_bytes) {
            uint32_t offset=state->eip-entry->guest_pc;
            while(intra<entry->instructions &&
                  entry->instruction_ends[intra]<=offset) intra++;
        }
        report->retired=state->step_retired + intra;
        engine->dispatcher_transitions++;
    }
    engine->retired_instructions+=report->retired;
    return invoked==PW_ERR_X87_TRAP?PW_ERR_X87_TRAP:invoked?PW_ERR_VM:PW_OK;
}

int pw_x86_engine_set_dispatch_profile(PwX86Engine *engine, unsigned enabled)
{
    if(!engine || !engine->initialized)return PW_ERR_PRECONDITION;
    engine->dispatch_profile=!!enabled;
    return PW_OK;
}

int pw_x86_engine_set_execution_clock(PwX86Engine *engine, PwX86ExecutionClock clock, void *opaque, uint32_t stride)
{
    if(!engine || !engine->initialized || (clock && !stride))return PW_ERR_PRECONDITION;
    engine->execution_clock=clock;
    engine->execution_clock_opaque=opaque;
    engine->execution_stride=stride;
    engine->execution_random=0x9e3779b9u;
    return PW_OK;
}

int pw_x86_engine_reset(PwX86Engine *engine,uint32_t generation)
{
    if(!engine || !engine->initialized)return PW_ERR_PRECONDITION;
    unsigned was_sealed=engine->sealed;
    const size_t discarded_bytes=engine->cache.cursor;
    if((engine->sealed || engine->failed) &&
       protection(engine,0,engine->code.bytes,PW_PROT_READ|PW_PROT_WRITE)!=PW_OK)
        return PW_ERR_VM;

    /* Count unlinks before cache entries are zeroed */
    for(uint32_t next=engine->cache.reset_head; next; next=engine->cache.entries[next-1].reset_next) {
        uint32_t i=next-1;
        if(engine->cache.entries[i].used) {
            if(engine->cache.entries[i].link_slots[0].is_linked) engine->unlinks++;
            if(engine->cache.entries[i].link_slots[1].is_linked) engine->unlinks++;
        }
    }

    int status=pw_x86_cache_reset(&engine->cache,generation);
    if(status!=PW_OK) {
        if(was_sealed && protection(engine,0,engine->code.bytes,
                                    PW_PROT_READ|PW_PROT_EXEC)!=PW_OK)
            engine->failed=1;
        return status;
    }
    /* Trap the discarded code, including its alignment padding. Unused arena
     * pages have never held published entry points and need no reset writes.
     * Poisoning the whole reserved arena made short flushes write 128 MiB. */
    if(discarded_bytes) memset(engine->code.write_base,0xcc,discarded_bytes);
    /* Every indirect target pointed into the code just discarded. */
    if(engine->indirect_targets)
        memset(engine->indirect_targets,0,PW_X86_ENGINE_INDIRECT_SLOTS*sizeof(PwX86IndirectTarget));
    if(engine->chain_targets)
        memset(engine->chain_targets,0,PW_X86_REENCODE_CHAIN_ENTRIES*sizeof(PwX86IndirectTarget));
    if(engine->block_map)
        memset(engine->block_map,0,
               ((discarded_bytes+PW_X86_ENGINE_FAULT_GRANULE-1)/PW_X86_ENGINE_FAULT_GRANULE)*sizeof(uint32_t));
    engine->last_published=0;
    engine->dispatches=0;engine->retired_instructions=0;engine->failed=0;
    return emit_return_stub(engine);
}

int pw_x86_engine_destroy(PwX86Engine *engine)
{
    if(!engine || !engine->initialized || !engine->backend)return PW_ERR_PRECONDITION;
    int status=engine->backend->release(engine->backend->context,&engine->code);
    if(status==PW_OK && engine->indirect_targets)
        status=engine->backend->release(engine->backend->context,&engine->indirect);
    if(status==PW_OK && engine->chain_targets)
        status=engine->backend->release(engine->backend->context,&engine->chain);
    if(status==PW_OK && engine->block_map)
        status=engine->backend->release(engine->backend->context,&engine->block_map_region);
    if(status==PW_OK)memset(engine,0,sizeof(*engine));
    return status;
}
