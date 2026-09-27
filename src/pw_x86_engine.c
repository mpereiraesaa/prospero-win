/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_x86_engine.h"
#include "pw_x86_reencode.h"
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

/* The bucket an exit waits in until its target PC is compiled: the target's
 * home slot in the cache's hash. */
static uint32_t target_bucket(const PwX86Cache *cache,uint32_t target_pc)
{
    uint32_t hash=target_pc*2654435761u;hash^=hash>>16;
    return hash%cache->capacity;
}

static void wait_for_target(PwX86Cache *cache,PwX86CacheEntry *entry,unsigned side)
{
    if(entry->link_slots[side].is_linked)return;
    uint32_t *head=&cache->entries[target_bucket(cache,entry->link_slots[side].target_pc)].pending_head;
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
    engine->reencode_enabled = enabled ? 1 : 0;
    return PW_OK;
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
        engine->residency_enabled ? engine->global_resident : (uint8_t)0 };
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
            if(pw_x86_cache_lookup_mut(&engine->cache, best.exit.target_pc, &tgt) == PW_OK) {
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
                engine->successful_links++;
            }
            if(best.exit.kind == PW_X86_EXIT_CONDITIONAL) {
                if(pw_x86_cache_lookup_mut(&engine->cache, best.exit.fallthrough_pc, &tgt) == PW_OK) {
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
        uint32_t *link = &engine->cache.entries[target_bucket(&engine->cache, pc)].pending_head;
        while(*link) {
            unsigned side = (*link - 1) & 1;
            PwX86CacheEntry *cand = &engine->cache.entries[(*link - 1) >> 1];
            PwX86LinkSlot *slot = &cand->link_slots[side];
            uint32_t *next = &cand->pending_next[side];

            if(!slot->is_linked && slot->target_pc != pc) { link = next; continue; }
            if(!slot->is_linked) {
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

    /* Dynamic unlinked chain resolution: if previous step exited via an unlinked slot, link it now */
    if(engine->chaining_enabled && state->last_exit_slot) {
        PwX86LinkSlot *last_slot = (PwX86LinkSlot *)state->last_exit_slot;
        if(last_slot->target_pc == state->eip && !last_slot->is_linked) {
            PwX86CacheEntry *target_entry = NULL;
            if(pw_x86_cache_lookup_mut(&engine->cache, state->eip, &target_entry) == PW_OK) {
                PwX86CacheEntry *source_entry = NULL;
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
                engine->successful_links++;
            }
        }
    }

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
        PwX86IndirectTarget *target=&engine->indirect_targets[
            pw_x86_indirect_slot(entry->guest_pc,PW_X86_ENGINE_INDIRECT_SLOTS-1)];
        target->guest_pc=entry->guest_pc;
        target->host_code=(const uint8_t *)engine->code.exec_base+entry->code_offset+
                          entry->canonical_entry_offset;
    }

    state->chain_budget = (engine->chaining_enabled && engine->quantum) ? engine->quantum : 1;
    state->step_retired = 0;
    state->step_transitions = 0;
    state->last_exit_slot = 0;
    state->reg_loads = 0;
    state->reg_stores = 0;
    state->reg_reconciliations = 0;
    state->reg_spills = 0;

    int invoked=invoke((uint8_t *)engine->code.exec_base+entry->code_offset+entry->canonical_entry_offset,state);

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

int pw_x86_engine_reset(PwX86Engine *engine,uint32_t generation)
{
    if(!engine || !engine->initialized)return PW_ERR_PRECONDITION;
    unsigned was_sealed=engine->sealed;
    if((engine->sealed || engine->failed) &&
       protection(engine,0,engine->code.bytes,PW_PROT_READ|PW_PROT_WRITE)!=PW_OK)
        return PW_ERR_VM;

    /* Count unlinks before cache entries are zeroed */
    for(uint32_t i=0; i<engine->cache.capacity; i++) {
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
    memset(engine->code.write_base,0xcc,engine->code.bytes);
    /* Every indirect target pointed into the code just discarded. */
    if(engine->indirect_targets)
        memset(engine->indirect_targets,0,PW_X86_ENGINE_INDIRECT_SLOTS*sizeof(PwX86IndirectTarget));
    engine->dispatches=0;engine->retired_instructions=0;engine->failed=0;return PW_OK;
}

int pw_x86_engine_destroy(PwX86Engine *engine)
{
    if(!engine || !engine->initialized || !engine->backend)return PW_ERR_PRECONDITION;
    int status=engine->backend->release(engine->backend->context,&engine->code);
    if(status==PW_OK && engine->indirect_targets)
        status=engine->backend->release(engine->backend->context,&engine->indirect);
    if(status==PW_OK)memset(engine,0,sizeof(*engine));
    return status;
}
