/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* wowprospero's per-thread translator budget (wine/wowprospero/thread_budget.h),
 * and a second guest thread brought up within it the way unix.c does. */
#include "../wine/wowprospero/thread_budget.h"
#include "../wine/wowprospero/host_memory.h"
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
    assert(b.entries == 65536 && b.arena_bytes == 128u << 20 && b.hostexec_bytes == 4u << 20);
    assert(!pw_wow_thread_budget(0, 0, &b));
    assert(b.entries == 16384 && b.arena_bytes == 32u << 20 && b.hostexec_bytes == 1u << 20);
    /* Each retry halves everything. */
    assert(!pw_wow_thread_budget(1, 1, &b));
    assert(b.entries == 32768 && b.arena_bytes == 64u << 20 && b.hostexec_bytes == 2u << 20);
    assert(!pw_wow_thread_budget(0, 2, &b));
    assert(b.entries == 4096 && b.arena_bytes == 8u << 20 && b.hostexec_bytes == 256u << 10);
    /* The first thread goes 128, 64, ... 2 MiB; a later one 32, 16, ... 2. */
    assert(!pw_wow_thread_budget(1, 6, &b) && b.arena_bytes == 2u << 20 && b.entries == 1024);
    assert(pw_wow_thread_budget(1, 7, &b) == -1);
    assert(!pw_wow_thread_budget(0, 4, &b) && b.arena_bytes == 2u << 20 && b.entries == 1024);
    assert(pw_wow_thread_budget(0, 5, &b) == -1);
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

    /* A later thread with little memory left settles for 4 MiB. */
    assert(pw_wow_thread_fit(0, arena_within, &limit, &used) == 3);
    assert(limit.calls == 4 && limit.seen[0] == 32u << 20 && limit.seen[3] == 4u << 20);
    assert(used.arena_bytes == 4u << 20 && used.entries == 2048);
    /* Everything refused: -1 after the floor, nothing reported as used. */
    limit = (Limit){ .arena_limit = 1u << 20 };
    memset(&used, 0xa5, sizeof(used));
    assert(pw_wow_thread_fit(1, arena_within, &limit, &used) == -1);
    assert(limit.calls == 7 && limit.seen[6] == 2u << 20 && used.entries == 0xa5a5a5a5u);
    /* The first budget that fits is taken as is. */
    limit = (Limit){ .arena_limit = 128u << 20 };
    assert(pw_wow_thread_fit(1, arena_within, &limit, &used) == 0 && limit.calls == 1);
}

/* A VM backend that refuses reservations above a size, like memory that
 * has run out. */
static PwVmBackend host;
static size_t reserve_limit;
static unsigned refused;

static int limited_reserve(void *context, size_t bytes, size_t alignment, PwVmRegion *out)
{
    if (bytes > reserve_limit) { refused++; return PW_ERR_VM; }
    return host.reserve(context, bytes, alignment, out);
}

/* Wine's side of host_memory.h: read-write-execute anonymous memory. */
static unsigned allocations, releases;
static void *allocate_rwx(size_t bytes)
{
    void *base = mmap(NULL, bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return NULL;
    allocations++;
    return base;
}
static void release_rwx(void *base, size_t bytes)
{
    assert(!munmap(base, bytes));
    releases++;
}
static PwWowHostMemory host_memory = { allocate_rwx, release_rwx, 4096, 4096 };

static void test_host_memory(void)
{
    PwVmBackend backend;
    PwVmRegion region;
    PwWowHostMemory broken = host_memory;

    broken.page = 3000;
    assert(pw_wow_host_backend(&backend, &broken) == PW_ERR_PRECONDITION);
    broken = host_memory;
    broken.alignment = 1024;
    assert(pw_wow_host_backend(NULL, &host_memory) == PW_ERR_PRECONDITION);
    assert(pw_wow_host_backend(&backend, &broken) == PW_ERR_PRECONDITION);
    assert(pw_wow_host_backend(&backend, &host_memory) == PW_OK && pw_vm_backend_valid(&backend));
    assert(backend.capabilities == PW_VM_CAP_PROTECT && !backend.reserve_at);
    /* Rounded to whole pages, one mapping for writing and running. */
    assert(backend.reserve(backend.context, 5000, 4096, &region) == PW_OK);
    assert(region.bytes == 8192 && region.write_base == region.exec_base && allocations == 1);
    /* Committed and executable already: protect only checks the range. */
    assert(backend.protect(backend.context, &region, 4096, 4096, PW_PROT_READ | PW_PROT_EXEC) == PW_OK);
    assert(backend.commit(backend.context, &region, 4096, 8192, PW_PROT_READ) == PW_ERR_PRECONDITION);
    ((uint8_t *)region.write_base)[8191] = 0xc3;
    assert(backend.release(backend.context, &region) == PW_OK && releases == 1);
    assert(backend.release(backend.context, &region) == PW_ERR_PRECONDITION && releases == 1);
    /* More alignment than the allocator gives, no size, or no room. */
    assert(backend.reserve(backend.context, 4096, 65536, &region) == PW_ERR_PRECONDITION);
    assert(backend.reserve(backend.context, 0, 4096, &region) == PW_ERR_PRECONDITION);
    assert(backend.reserve(backend.context, SIZE_MAX, 4096, &region) == PW_ERR_OVERFLOW);
    assert(backend.reserve(backend.context, (size_t)1 << 62, 4096, &region) == PW_ERR_VM);
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

    /* Room for 5 MiB: the 32, 16 and 8 MiB arenas are refused, 4 MiB is
     * taken. */
    reserve_limit = 5u << 20;
    refused = 0;
    memset(&r, 0, sizeof(r));
    assert(!pthread_create(&thread, NULL, guest_thread, &r) && !pthread_join(thread, NULL));
    assert(r.attempt == 3 && refused == 3 && r.used.arena_bytes == 4u << 20);
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
    test_budgets();
    test_fit();
    test_host_memory();
    /* The translator in Wine's memory, as unix.c sets it up, then over
     * plain mappings with protection changes, as before. */
    assert(pw_wow_host_backend(&host, &host_memory) == PW_OK);
    test_second_thread();
    assert(allocations == releases && allocations > 1);
    assert(pw_vm_posix_backend(&host) == PW_OK);
    test_second_thread();
    printf("wow thread budget passed: first and later budgets, halving to the floor, fitting into "
           "limited memory, Wine's read-write-execute memory, a second thread's first block and fs "
           "access, a guard miss as a fault\n");
    return 0;
}
