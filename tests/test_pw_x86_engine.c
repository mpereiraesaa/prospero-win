/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_x86_engine.h"
#include "../src/pw_x86_reencode.h"
#include "../src/pw_vm_posix.h"
#include <assert.h>
/* Fixed-width aliases for the shared PE/Unix reporting contract. */
typedef uint64_t UINT64;
typedef uint32_t UINT;
typedef int32_t INT;
#include "../wine/wowprospero/wowprospero.h"

static uint64_t clock_now;
static unsigned clock_reads, clock_failed;
static uint64_t execution_clock(void *opaque)
{
    assert(opaque==&clock_now);
    clock_reads++;
    clock_now+=10;
    return clock_failed?0:clock_now;
}
typedef struct Source { uint32_t base;const uint8_t *data;size_t bytes; } Source;
static int source_view(void *opaque,uint32_t pc,const uint8_t **data,size_t *bytes)
{
    Source *s=opaque;
    clock_now+=1000; /* Simulated translation work must not count as execution. */
    if(pc<s->base || (uint64_t)pc>=s->base+s->bytes)return PW_ERR_NOT_FOUND;
    size_t offset=pc-s->base;*data=s->data+offset;*bytes=s->bytes-offset;return PW_OK;
}

typedef struct BatchClock {
    uint64_t now,step,quantum;
    unsigned reads,fail_at,backwards_at;
} BatchClock;
static uint64_t batch_clock(void *opaque)
{
    BatchClock *clock=opaque;
    clock->reads++;
    clock->now+=clock->step;
    if(clock->reads==clock->fail_at)return 0;
    if(clock->reads==clock->backwards_at)return 1;
    return clock->now/clock->quantum*clock->quantum;
}
static void test_execution_clock_batch(void)
{
    uint64_t mean=123;
    assert(pw_x86_execution_clock_batch(NULL,NULL,&mean)==PW_ERR_PRECONDITION);
    BatchClock clock={.now=1000000,.step=10,.quantum=1};
    assert(pw_x86_execution_clock_batch(batch_clock,&clock,NULL)==PW_ERR_PRECONDITION);
    assert(pw_x86_execution_clock_batch(batch_clock,&clock,&mean)==PW_OK && mean==10);
    /* 1 us granularity must not turn a 3 ns read into a 1000 ns correction. */
    clock=(BatchClock){.now=1000000,.step=3,.quantum=1000};
    assert(pw_x86_execution_clock_batch(batch_clock,&clock,&mean)==PW_OK && mean>=2 && mean<=4);
    clock=(BatchClock){.now=1000000,.quantum=1000};
    assert(pw_x86_execution_clock_batch(batch_clock,&clock,&mean)==PW_OK && mean==0);
    clock=(BatchClock){.now=1000000,.step=10,.quantum=1,.fail_at=513};
    assert(pw_x86_execution_clock_batch(batch_clock,&clock,&mean)==PW_ERR_VM && mean==0);
    clock=(BatchClock){.now=1000000,.step=10,.quantum=1,.backwards_at=513};
    assert(pw_x86_execution_clock_batch(batch_clock,&clock,&mean)==PW_ERR_VM && mean==0);
}

static void test_refused_reset_execution(void)
{
    const uint8_t loop[]={0x40,0xeb,0xfd};
    Source source={0x1000,loop,sizeof(loop)};
    PwVmBackend vm;PwX86Engine engine;PwX86CacheEntry entries[8];
    PwX86State state={.eip=0x1000};PwX86StepReport step;
    assert(pw_vm_posix_backend(&vm)==PW_OK);
    assert(pw_x86_engine_init(&engine,&vm,entries,8,32u<<20,1,source_view,&source)==PW_OK);
    assert(pw_x86_engine_step(&engine,&state,&step)==PW_OK && state.gpr[0]==1);
    const size_t cursor=engine.cache.cursor;
    assert(cursor && cursor<vm.page_bytes && engine.sealed && !engine.failed);
    /* Real mprotect removes execute permission during reset. Both refused
     * generations must restore RX so a cache hit can execute the old block. */
    for(unsigned generation=0;generation<2;generation++) {
        const uint64_t calls=engine.protection_calls,bytes=engine.protection_bytes;
        assert(pw_x86_engine_reset(&engine,generation)==PW_ERR_PRECONDITION);
        assert(engine.cache.generation==1 && engine.cache.cursor==cursor &&
               engine.cache.occupied==1 && engine.cache.publishes==1 && !engine.cache.resets);
        assert(pw_x86_engine_step(&engine,&state,&step)==PW_OK && step.cache_hit);
        assert(engine.sealed && !engine.failed && engine.protection_calls==calls+2 &&
               engine.protection_bytes==bytes+2*vm.page_bytes);
        assert(state.eip==0x1000 && state.gpr[0]==generation+2 && engine.compiles==1);
    }
    assert(pw_x86_engine_reset(&engine,2)==PW_OK && !engine.sealed);
    assert(pw_x86_engine_step(&engine,&state,&step)==PW_OK && !step.cache_hit);
    assert(state.gpr[0]==4 && engine.sealed && engine.compiles==2);
    assert(pw_x86_engine_destroy(&engine)==PW_OK);
}

static void test_empty_chain_reset(void)
{
    const uint8_t jump[]={0xff,0xe2}; /* jmp edx, which is zero */
    const uint8_t body[]={0x40}; /* finite code at PC zero */
    Source source={0x1000,jump,sizeof(jump)};
    PwVmBackend vm;PwX86Engine engine;PwX86CacheEntry entries[8];
    PwX86State state={.eip=0x1000};PwX86StepReport step;
    assert(pw_vm_posix_backend(&vm)==PW_OK);
    assert(pw_x86_engine_init(&engine,&vm,entries,8,16384,1,source_view,&source)==PW_OK);
    assert(pw_x86_engine_set_chaining(&engine,1)==PW_OK);
    assert(pw_x86_engine_set_quantum(&engine,1)==PW_OK);
    assert(pw_x86_engine_set_indirect(&engine,1)==PW_OK);
    assert(pw_x86_engine_set_counters(&engine,0)==PW_OK);
    assert(pw_x86_engine_set_flat_memory(&engine,0x1000,0x20000)==PW_OK);
    assert(pw_x86_engine_set_reencode(&engine,1)==PW_OK);
    assert(pw_x86_engine_set_unbounded_chains(&engine,1)==PW_OK);
    assert(pw_x86_engine_step(&engine,&state,&step)==PW_OK && state.eip==0);
    {
        const int status=pw_x86_engine_step(&engine,&state,&step);
        struct pw_wow_run_params result={0};
        assert(status==PW_ERR_NOT_FOUND);
        pw_wow_report_error(&result,status,state.eip,0xdead1234u,1);
        assert(result.reason==PW_WOW_FAULT && result.status==status &&
               result.fault_address==0 && result.fault_write==0);
    }
    /* A real PC-zero translation replaces the empty sentinel. */
    source=(Source){0,body,sizeof(body)};
    assert(pw_x86_engine_step(&engine,&state,&step)==PW_OK);
    assert(state.gpr[0]==1 && state.eip==1 && engine.chain_targets[0].host_code &&
           engine.chain_targets[0].guest_pc==0);
    for (uint64_t generation=2; generation<4; generation++) {
        assert(pw_x86_engine_reset(&engine,generation)==PW_OK);
        assert(engine.chain_targets[0].guest_pc==1 && !engine.chain_targets[0].host_code);
        assert(engine.chain_targets[PW_X86_REENCODE_CHAIN_SLOTS].guest_pc==1 &&
               !engine.chain_targets[PW_X86_REENCODE_CHAIN_SLOTS].host_code);
        source=(Source){0x1000,jump,sizeof(jump)};
        state.eip=0x1000;
        assert(pw_x86_engine_step(&engine,&state,&step)==PW_OK && state.eip==0);
        assert(pw_x86_engine_step(&engine,&state,&step)==PW_ERR_NOT_FOUND);
        /* The sentinel also permits PC zero to be published after reset. */
        source=(Source){0,body,sizeof(body)};
        assert(pw_x86_engine_step(&engine,&state,&step)==PW_OK && state.eip==1);
        assert(state.gpr[0]==generation && engine.chain_targets[0].host_code &&
               engine.chain_targets[0].guest_pc==0);
    }
    assert(pw_x86_engine_destroy(&engine)==PW_OK);
}

static void test_guest_error_reporting(void)
{
    struct pw_wow_run_params result={0};
    pw_wow_report_error(&result,PW_ERR_NOT_FOUND,0xfffff000u,0xdead1234u,1);
    assert(result.reason==PW_WOW_FAULT && result.fault_address==0xfffff000u && !result.fault_write);
    pw_wow_report_error(&result,PW_ERR_VM,0x1234,0x5678,1);
    assert(result.reason==PW_WOW_FAULT && result.fault_address==0x5678 && result.fault_write==1);
    pw_wow_report_error(&result,PW_ERR_UNSUPPORTED,0x1234,0,0);
    assert(result.reason==PW_WOW_UNSUPPORTED);
    pw_wow_report_error(&result,PW_ERR_X87_TRAP,0x1234,0,0);
    assert(result.reason==PW_WOW_X87_TRAP);
    pw_wow_report_error(&result,PW_ERR_STATE,0x1234,0,0);
    assert(result.reason==PW_WOW_ERROR);
}

static void test_dispatch_profile(void)
{
    const uint8_t loop[]={0x40,0xeb,0xfd};
    Source source={0x1000,loop,sizeof(loop)};
    PwVmBackend vm;PwX86Engine engine;PwX86CacheEntry entries[8];
    PwX86State state={.eip=0x1000};PwX86StepReport step;
    assert(pw_x86_engine_set_dispatch_profile(NULL,1)==PW_ERR_PRECONDITION);
    assert(pw_vm_posix_backend(&vm)==PW_OK);
    assert(pw_x86_engine_init(&engine,&vm,entries,8,16384,1,source_view,&source)==PW_OK);
    assert(pw_x86_engine_set_chaining(&engine,1)==PW_OK);
    assert(pw_x86_engine_set_quantum(&engine,1)==PW_OK);
    assert(pw_x86_engine_set_indirect(&engine,1)==PW_OK);
    assert(pw_x86_engine_set_counters(&engine,0)==PW_OK);
    assert(pw_x86_engine_set_global_resident(&engine,0xfb)==PW_OK);
    assert(pw_x86_engine_set_flat_memory(&engine,0x1000,0x40000)==PW_OK);
    assert(pw_x86_engine_set_reencode(&engine,1)==PW_OK);
    assert(pw_x86_engine_step(&engine,&state,&step)==PW_OK);
    assert(engine.chain_targets[0x1000].host_code &&
           engine.chain_targets[0x1000].guest_pc==0x1000);
    assert(!engine.dispatch_chain_matches && !engine.dispatch_chain_empty &&
           !engine.dispatch_chain_collisions); /* Default off. */
    assert(pw_x86_engine_set_dispatch_profile(&engine,1)==PW_OK);
    assert(pw_x86_engine_step(&engine,&state,&step)==PW_OK);
    assert(engine.dispatch_chain_matches==1);
    assert(pw_x86_chain_slot(0x1000)!=pw_x86_chain_slot(0x11000));
    assert(pw_x86_chain_slot(0x1000)==pw_x86_chain_slot(0x10f00));
    assert(pw_x86_chain_slot(0x1000)==pw_x86_chain_slot(0x20e00));
    /* Real compiled colliding targets occupy different banks. */
    source.base=state.eip=0x10f00;
    assert(pw_x86_engine_step(&engine,&state,&step)==PW_OK);
    assert(engine.chain_targets[0x1000].guest_pc==0x10f00);
    assert(engine.chain_targets[0x1000+PW_X86_REENCODE_CHAIN_SLOTS].guest_pc==0x1000);
    source.base=state.eip=0x1000;
    assert(pw_x86_engine_step(&engine,&state,&step)==PW_OK && step.cache_hit);
    assert(engine.dispatch_chain_collisions==1);
    assert(state.gpr[0]==4); /* Profiling preserves the executed loop. */
    /* A third collider displaces the oldest bank; republishing an evicted
     * cache entry must not retain a dangling or incorrect target. */
    source.base=state.eip=0x20e00;
    assert(pw_x86_engine_step(&engine,&state,&step)==PW_OK);
    assert(engine.chain_targets[0x1000].guest_pc==0x20e00);
    assert(engine.chain_targets[0x1000+PW_X86_REENCODE_CHAIN_SLOTS].guest_pc==0x10f00);
    source.base=state.eip=0x1000;
    assert(pw_x86_engine_step(&engine,&state,&step)==PW_OK && step.cache_hit);
    assert(engine.chain_targets[0x1000].guest_pc==0x1000);
    assert(engine.chain_targets[0x1000+PW_X86_REENCODE_CHAIN_SLOTS].guest_pc==0x20e00);
    assert(engine.dispatch_chain_collisions==3 && state.gpr[0]==6);
    assert(pw_x86_engine_reset(&engine,2)==PW_OK);
    assert(!engine.chain_targets[0x1000].host_code &&
           !engine.chain_targets[0x1000+PW_X86_REENCODE_CHAIN_SLOTS].host_code);
    state.eip=0x1000;
    assert(pw_x86_engine_step(&engine,&state,&step)==PW_OK);
    assert(engine.dispatch_chain_empty==1 && engine.dispatch_chain_collisions==3 &&
           engine.dispatch_chain_matches==2); /* Cumulative over reset. */
    assert(pw_x86_engine_set_dispatch_profile(&engine,0)==PW_OK);
    assert(pw_x86_engine_step(&engine,&state,&step)==PW_OK);
    assert(engine.dispatch_chain_matches==2 && state.gpr[0]==8);
    assert(pw_x86_engine_destroy(&engine)==PW_OK);
}

int main(void)
{
    const uint8_t loop[]={0x40,0xeb,0xfd}; /* inc eax; jmp to block start */
    Source source={0x1000,loop,sizeof(loop)};PwVmBackend vm;PwX86Engine engine;
    PwX86CacheEntry entries[8];PwX86State state={.eip=0x1000};PwX86StepReport step;
    assert(pw_vm_posix_backend(&vm)==PW_OK);
    assert(pw_x86_engine_init(&engine,&vm,entries,8,4096,1,source_view,&source)==PW_OK);
    assert(pw_x86_engine_set_execution_clock(&engine,execution_clock,&clock_now,0)==PW_ERR_PRECONDITION);
    assert(pw_x86_engine_set_execution_clock(&engine,execution_clock,&clock_now,1)==PW_OK);
    assert(pw_x86_engine_step(&engine,&state,&step)==PW_OK);
    assert(step.instructions==2 && step.retired==2 && !step.cache_hit);
    assert(state.eip==0x1000 && state.gpr[0]==1 && engine.cache.publishes==1);
    assert(engine.compiles==1 && engine.protection_calls==2 &&
           engine.protection_bytes==8192);
    assert(engine.execution_ns==10 && engine.execution_calls==1 && clock_reads==2);
    assert(pw_x86_engine_step(&engine,&state,&step)==PW_OK);
    assert(step.instructions==2 && step.retired==2 && step.cache_hit);
    assert(state.eip==0x1000 && state.gpr[0]==2 && engine.cache.hits==1);
    assert(engine.dispatches==2 && engine.retired_instructions==4);
    assert(engine.compiles==1 && engine.protection_calls==2);
    assert(engine.execution_ns==20 && engine.execution_calls==2 && clock_reads==4);

    {
        size_t used=engine.cache.cursor;
        uint8_t *code=engine.code.write_base;
        assert(used && used<engine.code.bytes);
        assert(vm.protect(vm.context,&engine.code,0,engine.code.bytes,PW_PROT_READ|PW_PROT_WRITE)==PW_OK);
        code[used]=0x5a;
        code[engine.code.bytes-1]=0xa5;
        assert(pw_x86_engine_reset(&engine,2)==PW_OK);
        for(size_t i=0;i<used;i++)assert(code[i]==0xcc);
        assert(code[used]==0x5a && code[engine.code.bytes-1]==0xa5);
    }
    state.eip=0x1000;
    assert(pw_x86_engine_step(&engine,&state,&step)==PW_OK && !step.cache_hit);
    assert(engine.cache.generation==2 && engine.cache.publishes==2 && engine.cache.resets==1);
    assert(engine.execution_ns==30 && engine.execution_calls==3);

    const uint8_t fault[]={0xbc,0,0,0,0,0x50}; /* mov esp,0; push esp */
    source=(Source){0x2000,fault,sizeof(fault)};
    assert(pw_x86_engine_reset(&engine,3)==PW_OK);state=(PwX86State){.eip=0x2000};
    assert(pw_x86_engine_step(&engine,&state,&step)==PW_ERR_VM);
    assert(step.instructions==2 && step.retired==1 && state.eip==0x2005);
    assert(engine.retired_instructions==1);
    assert(engine.execution_ns==40 && engine.execution_calls==4); /* Faulting invocation included. */
    clock_failed=1;

    const uint8_t x87_trap[]={0xd9,0xe8,0xd9,0xee,0xde,0xf9};
    source=(Source){0x3000,x87_trap,sizeof(x87_trap)};
    assert(pw_x86_engine_reset(&engine,4)==PW_OK);
    state=(PwX86State){.eip=0x3000};pw_guest_fp_init(&state.fp);
    state.fp.x87_control=(uint16_t)(state.fp.x87_control&~4u);
    assert(pw_x86_engine_step(&engine,&state,&step)==PW_ERR_X87_TRAP);
    assert(step.instructions==3 && step.retired==2 && state.eip==0x3004 &&
           state.fp.x87_pending==4 && (state.fp.x87_status&0x84)==0x84);
    uint8_t value[10];assert(pw_guest_x87_peek(&state.fp,0,value)==PW_OK);
    assert(pw_guest_x87_peek(&state.fp,1,value)==PW_OK);
    assert(engine.retired_instructions==2);
    assert(engine.execution_ns==40 && engine.execution_calls==5 && engine.execution_clock_errors==1);
    assert(pw_x86_engine_set_execution_clock(&engine,NULL,NULL,0)==PW_OK);
    unsigned reads=clock_reads;

    const uint8_t x87_stack_trap[]={0xd9,0xe8,0xd9,0xe8,0xd9,0xe8,0xd9,0xe8,
        0xd9,0xe8,0xd9,0xe8,0xd9,0xe8,0xd9,0xe8,0xd9,0xe8};
    source=(Source){0x4000,x87_stack_trap,sizeof(x87_stack_trap)};
    assert(pw_x86_engine_reset(&engine,5)==PW_OK);
    state=(PwX86State){.eip=0x4000};pw_guest_fp_init(&state.fp);
    state.fp.x87_control=(uint16_t)(state.fp.x87_control&~1u);
    assert(pw_x86_engine_step(&engine,&state,&step)==PW_ERR_X87_TRAP);
    assert(step.instructions==9 && step.retired==8 && state.eip==0x4010 &&
           state.fp.x87_pending==1 && (state.fp.x87_status&0x02c1)==0x02c1 &&
           state.fp.x87_tag==0 && ((state.fp.x87_status>>11)&7)==0);
    assert(engine.retired_instructions==8);
    assert(clock_reads==reads && engine.execution_calls==5);
    clock_failed=0;
    assert(pw_x86_engine_set_execution_clock(&engine,execution_clock,&clock_now,64)==PW_OK);
    source=(Source){0x1000,loop,sizeof(loop)};
    assert(pw_x86_engine_reset(&engine,6)==PW_OK);
    state=(PwX86State){.eip=0x1000};
    for(unsigned i=0;i<1000;i++)assert(pw_x86_engine_step(&engine,&state,&step)==PW_OK);
    assert(state.gpr[0]==1000 && engine.execution_calls==1005);
    assert(engine.execution_samples>5 && engine.execution_samples<50);
    assert(clock_reads==2*engine.execution_samples);
    assert(engine.execution_ns==10*(engine.execution_samples-1)); /* Failed clock sample excluded. */
    assert(pw_x86_engine_destroy(&engine)==PW_OK);
    test_empty_chain_reset();
    test_refused_reset_execution();
    test_guest_error_reporting();
    test_execution_clock_batch();
    test_dispatch_profile();
    return 0;
}
