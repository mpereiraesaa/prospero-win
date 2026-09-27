/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* wowprospero's per-thread translator budget (wine/wowprospero/thread_budget.h),
 * and a second guest thread brought up within it the way unix.c does. */
#include "../wine/wowprospero/thread_budget.h"
#include "../src/pw_x86_engine.h"
#include "../src/pw_x86_hostexec.h"
#include "../src/pw_vm_posix.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static void test_budgets(void)
{
    PwWowThreadBudget b;

    assert(!pw_wow_thread_budget(1, 0, &b));
    assert(b.entries == 32768 && b.arena_bytes == 32u << 20 && b.hostexec_bytes == 4u << 20);
    assert(!pw_wow_thread_budget(0, 0, &b));
    assert(b.entries == 8192 && b.arena_bytes == 8u << 20 && b.hostexec_bytes == 1u << 20);
    /* Each retry halves everything. */
    assert(!pw_wow_thread_budget(1, 1, &b));
    assert(b.entries == 16384 && b.arena_bytes == 16u << 20 && b.hostexec_bytes == 2u << 20);
    assert(!pw_wow_thread_budget(0, 2, &b));
    assert(b.entries == 2048 && b.arena_bytes == 2u << 20 && b.hostexec_bytes == 256u << 10);
    /* The first thread goes 32, 16, 8, 4, 2 MiB; a later one 8, 4, 2. */
    assert(!pw_wow_thread_budget(1, 4, &b) && b.arena_bytes == 2u << 20 && b.entries == 2048);
    assert(pw_wow_thread_budget(1, 5, &b) == -1);
    assert(pw_wow_thread_budget(0, 3, &b) == -1);
}

typedef struct Limit { size_t arena_limit; unsigned calls; size_t seen[8]; } Limit;

static int arena_within(void *context, const PwWowThreadBudget *budget)
{
    Limit *limit = context;

    limit->seen[limit->calls++ & 7] = budget->arena_bytes;
    return budget->arena_bytes <= limit->arena_limit ? 0 : -1;
}

static void test_fit(void)
{
    Limit limit = { .arena_limit = 5u << 20 };
    PwWowThreadBudget used;

    /* A later thread in a nearly full address space settles for 4 MiB. */
    assert(pw_wow_thread_fit(0, arena_within, &limit, &used) == 1);
    assert(limit.calls == 2 && limit.seen[0] == 8u << 20 && limit.seen[1] == 4u << 20);
    assert(used.arena_bytes == 4u << 20 && used.entries == 4096);
    /* Everything refused: -1 after the floor, nothing reported as used. */
    limit = (Limit){ .arena_limit = 1u << 20 };
    memset(&used, 0xa5, sizeof(used));
    assert(pw_wow_thread_fit(1, arena_within, &limit, &used) == -1);
    assert(limit.calls == 5 && limit.seen[4] == 2u << 20 && used.entries == 0xa5a5a5a5u);
    /* The first budget that fits is taken as is. */
    limit = (Limit){ .arena_limit = 64u << 20 };
    assert(pw_wow_thread_fit(1, arena_within, &limit, &used) == 0 && limit.calls == 1);
}

/* A VM backend that refuses reservations above a size, like an address
 * space that has run out of room. */
static PwVmBackend host;
static size_t reserve_limit;
static unsigned refused;

static int limited_reserve(void *context, size_t bytes, size_t alignment, PwVmRegion *out)
{
    if (bytes > reserve_limit) { refused++; return PW_ERR_VM; }
    return host.reserve(context, bytes, alignment, out);
}

/* One guest thread's translator, as unix.c sets it up. */
typedef struct Thread {
    PwVmBackend vm;
    PwX86Engine engine;
    PwX86HostExec hostexec;
    PwX86CacheEntry *entries;
} Thread;

enum { SPAN = 0x40000, CODE = 0x1000, TEB = 0x30000, STACK_TOP = 0x3f000 };
static uint8_t *guest;
static uint32_t low;

static int view(void *opaque, uint32_t pc, const uint8_t **data, size_t *bytes)
{
    (void)opaque;
    if (pc < low + CODE || pc >= low + TEB) return PW_ERR_NOT_FOUND;
    *data = (const uint8_t *)(uintptr_t)pc;
    *bytes = low + TEB - pc;
    return PW_OK;
}

static int setup_real(void *context, const PwWowThreadBudget *budget)
{
    Thread *t = context;

    if (!(t->entries = calloc(budget->entries, sizeof(*t->entries)))) return -1;
    if (pw_x86_engine_init(&t->engine, &t->vm, t->entries, budget->entries, budget->arena_bytes,
                           1, view, NULL) == PW_OK) {
        if (pw_x86_hostexec_init(&t->hostexec, &t->vm, budget->hostexec_bytes) == PW_OK) return 0;
        pw_x86_engine_destroy(&t->engine);
    }
    free(t->entries);
    t->entries = NULL;
    return -1;
}

typedef struct Result { int attempt; PwWowThreadBudget used; int status, fault_status;
                        uint32_t eax, eip, fault_address; } Result;

/* A guest thread's life: fit its translator into what is left, read its
 * TEB through fs, then touch memory below the guest range. */
static void *guest_thread(void *opaque)
{
    Result *r = opaque;
    Thread t;
    PwX86State state;
    PwX86StepReport step;

    memset(&t, 0, sizeof(t));
    t.vm = host;
    t.vm.reserve = limited_reserve;
    r->attempt = pw_wow_thread_fit(0, setup_real, &t, &r->used);
    if (r->attempt < 0) return NULL;
    assert(pw_x86_engine_set_chaining(&t.engine, 1) == PW_OK);
    assert(pw_x86_engine_set_counters(&t.engine, 0) == PW_OK);
    assert(pw_x86_engine_set_flat_memory(&t.engine, low, low + SPAN) == PW_OK);
    assert(pw_x86_engine_set_reencode(&t.engine, 1) == PW_OK);
    memset(&state, 0, sizeof(state));
    state.eip = low + CODE;
    state.gpr[4] = low + STACK_TOP;
    state.eflags = 0x2;
    state.fs_base = low + TEB;
    state.fs_bytes = 0x1000;
    state.stack_low = low;
    state.stack_high = low + SPAN;
    state.memory_count = 1;
    state.memory[0].low = low;
    state.memory[0].high = (uint64_t)low + SPAN;
    state.memory[0].permissions = PW_X86_READ | PW_X86_WRITE;
    /* The thread's first block: mov eax, fs:[0x18] (the TEB's self pointer);
     * ret to a sentinel. */
    r->status = PW_OK;
    for (unsigned i = 0; i < 1000 && state.eip != 0xdead0000u; i++)
        if ((r->status = pw_x86_engine_step(&t.engine, &state, &step)) != PW_OK) break;
    r->eax = state.gpr[0];
    r->eip = state.eip;
    /* Then mov eax, [8]: below the guest range, a guest access violation
     * (PW_ERR_VM with the address; wowprospero raises it), not a fatal
     * translator error. */
    state.eip = low + CODE + 0x10;
    r->fault_status = pw_x86_engine_step(&t.engine, &state, &step);
    r->fault_address = state.fault_address;
    assert(pw_x86_hostexec_destroy(&t.hostexec) == PW_OK);
    assert(pw_x86_engine_destroy(&t.engine) == PW_OK);
    free(t.entries);
    return NULL;
}

static void test_second_thread(void)
{
    static const uint8_t first_block[] = { 0x64, 0xa1, 0x18, 0x00, 0x00, 0x00, 0xc3 };
    static const uint8_t below_range[] = { 0xa1, 0x08, 0x00, 0x00, 0x00, 0xc3 };
    uint32_t self_pointer;
    pthread_t thread;
    Result r;

    guest = mmap(NULL, SPAN, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    assert(guest != MAP_FAILED);
    low = (uint32_t)(uintptr_t)guest;
    memset(guest + CODE, 0xcc, TEB - CODE);
    memcpy(guest + CODE, first_block, sizeof(first_block));
    memcpy(guest + CODE + 0x10, below_range, sizeof(below_range));
    self_pointer = low + TEB;
    memcpy(guest + TEB + 0x18, &self_pointer, 4);
    memcpy(guest + STACK_TOP, &(uint32_t){ 0xdead0000u }, 4);

    /* Room for 5 MiB: the 8 MiB arena is refused, 4 MiB is taken. */
    reserve_limit = 5u << 20;
    refused = 0;
    memset(&r, 0, sizeof(r));
    assert(!pthread_create(&thread, NULL, guest_thread, &r) && !pthread_join(thread, NULL));
    assert(r.attempt == 1 && refused == 1 && r.used.arena_bytes == 4u << 20);
    assert(r.status == PW_OK && r.eip == 0xdead0000u && r.eax == low + TEB);
    assert(r.fault_status == PW_ERR_VM && r.fault_address == 8);

    /* No room at all: the thread reports it has no translator instead of
     * leaving half of one reserved. */
    reserve_limit = 1u << 20;
    memset(&r, 0, sizeof(r));
    assert(!pthread_create(&thread, NULL, guest_thread, &r) && !pthread_join(thread, NULL));
    assert(r.attempt == -1);
    assert(munmap(guest, SPAN) == 0);
}

int main(void)
{
    assert(pw_vm_posix_backend(&host) == PW_OK);
    test_budgets();
    test_fit();
    test_second_thread();
    printf("wow thread budget passed: first and later budgets, halving to the floor, fitting into a "
           "limited address space, a second thread's first block and fs access, a guard miss as a fault\n");
    return 0;
}
