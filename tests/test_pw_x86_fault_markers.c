/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Re-encoded blocks with fault markers against the flat guard. The guest
 * accesses the host's unmapped first page, which the guard refuses as
 * outside the flat range; with markers the access faults natively, a
 * SIGSEGV handler sends it to pw_x86_engine_fault_redirect, and the step
 * must end exactly as with the guard: the same status, EIP, registers,
 * arithmetic flags, fault address and direction.
 */
#define _GNU_SOURCE
#include "../src/pw_x86_engine.h"
#include "../src/pw_x86_reencode.h"
#include "../src/pw_vm_posix.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>

enum { SPAN = 0x40000, CODE = 0x1000, DATA = 0x20000, STACK_TOP = 0x3f000 };

static uint8_t *guest;
static uint32_t low;
static PwX86Engine *current;
static unsigned redirected;

static int view(void *opaque, uint32_t pc, const uint8_t **data, size_t *bytes)
{
    (void)opaque;
    if (pc < low + CODE || pc >= low + DATA) return PW_ERR_NOT_FOUND;
    *data = (const uint8_t *)(uintptr_t)pc;
    *bytes = low + DATA - pc;
    return PW_OK;
}

static void on_fault(int sig, siginfo_t *info, void *context)
{
    ucontext_t *uc = context;
    uintptr_t target = current ? pw_x86_engine_fault_redirect(current, (uintptr_t)uc->uc_mcontext.gregs[REG_RIP]) : 0;

    (void)sig; (void)info;
    if (!target) {
        static const char message[] = "unexpected fault\n";
        (void)!write(2, message, sizeof(message) - 1);
        _exit(3);
    }
    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)target;
    redirected++;
}

typedef struct Run {
    PwX86State state;
    int status;
    uint64_t reencoded;
    unsigned redirected;
} Run;

static Run run(const uint8_t *code, size_t bytes, unsigned markers)
{
    static PwX86CacheEntry entries[512];
    static PwVmBackend vm;
    PwX86Engine engine;
    PwX86StepReport step;
    Run r;

    memset(guest + CODE, 0xcc, DATA - CODE);
    memcpy(guest + CODE, code, bytes);
    memset(&r, 0, sizeof(r));
    r.state.eip = low + CODE;
    r.state.stack_low = low;
    r.state.stack_high = low + SPAN;
    r.state.memory_count = 1;
    r.state.memory[0].low = low;
    r.state.memory[0].high = (uint64_t)low + SPAN;
    r.state.memory[0].permissions = PW_X86_READ | PW_X86_WRITE;
    for (unsigned g = 0; g < 8; g++) r.state.gpr[g] = 0x01010101u * (g + 3);
    r.state.gpr[4] = low + STACK_TOP;
    r.state.eflags = 0x2;
    memcpy(guest + STACK_TOP, &(uint32_t){ 0xdead0000u }, 4);
    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_engine_init(&engine, &vm, entries, 512, 1u << 20, 1, view, NULL) == PW_OK);
    assert(pw_x86_engine_set_chaining(&engine, 1) == PW_OK);
    assert(pw_x86_engine_set_indirect(&engine, 1) == PW_OK);
    assert(pw_x86_engine_set_counters(&engine, 0) == PW_OK);
    assert(pw_x86_engine_set_flat_memory(&engine, low, low + SPAN) == PW_OK);
    assert(pw_x86_engine_set_reencode(&engine, 1) == PW_OK);
    assert(pw_x86_engine_set_fault_markers(&engine, markers) == PW_OK);
    current = &engine;
    redirected = 0;
    r.status = PW_OK;
    for (unsigned i = 0; i < 1000 && r.state.eip != 0xdead0000u; i++)
        if ((r.status = pw_x86_engine_step(&engine, &r.state, &step)) != PW_OK) break;
    current = NULL;
    r.reencoded = engine.reencoded_blocks;
    r.redirected = redirected;
    assert(pw_x86_engine_destroy(&engine) == PW_OK);
    return r;
}

/* The guest program faults at the instruction at offset fault_at; both
 * modes report it identically, and only the marked one took a host fault.
 * flags < 0: the arithmetic flags match the guard's, which is exact when a
 * later instruction reads them. Otherwise the guard reports what its own
 * compare left (pw_x86_reencode.h), and the marked block the guest's real
 * flags, which are given. */
static void compare(const char *name, const uint8_t *code, size_t bytes, uint32_t fault_at,
                    uint32_t address, uint8_t write, int flags)
{
    Run guarded = run(code, bytes, 0), marked = run(code, bytes, 1);

    if (guarded.status != PW_ERR_VM || guarded.state.eip != low + CODE + fault_at)
        fprintf(stderr, "%s: guarded status %d eip +%x\n", name, guarded.status,
                guarded.state.eip - low - CODE);
    assert(guarded.status == PW_ERR_VM && guarded.state.eip == low + CODE + fault_at);
    assert(guarded.state.fault_address == address && guarded.state.fault_write == write);
    assert(guarded.reencoded && marked.reencoded && !guarded.redirected && marked.redirected == 1);
    assert(marked.status == guarded.status && marked.state.eip == guarded.state.eip);
    for (unsigned g = 0; g < 8; g++) assert(marked.state.gpr[g] == guarded.state.gpr[g]);
    {
        const uint32_t want = flags < 0 ? guarded.state.eflags & 0x8d5 : (uint32_t)flags;
        if ((marked.state.eflags & 0x8d5) != want)
            fprintf(stderr, "%s: flags %03x != %03x\n", name, marked.state.eflags & 0x8d5, want);
        assert((marked.state.eflags & 0x8d5) == want);
    }
    assert(marked.state.fault_address == address && marked.state.fault_write == write);
    assert(marked.state.fault_width == guarded.state.fault_width);
}

static void test_redirect_bounds(void)
{
    static const uint8_t code[32] = {
        [0] = 0x0f, 0x1f, 0x84, 0x00, 0x10, 0x00, 0x00, 0x00,   /* marker, +16 */
        [8] = 0x90,
        [16] = 0x0f, 0x1f, 0x84, 0x00, 0xf0, 0xff, 0xff, 0xff,  /* marker, backwards */
        [24] = 0x90,
    };
    const uintptr_t low_code = (uintptr_t)code, high = low_code + sizeof(code);

    assert(pw_x86_fault_redirect(low_code + 8, low_code, high) == low_code + 24);
    assert(!pw_x86_fault_redirect(low_code + 8, low_code, low_code + 20));  /* target past the end */
    assert(!pw_x86_fault_redirect(low_code + 24, low_code, high));           /* backwards */
    assert(!pw_x86_fault_redirect(low_code + 9, low_code, high));            /* no marker */
    assert(!pw_x86_fault_redirect(low_code + 4, low_code, high));            /* before the region */
    assert(!pw_x86_fault_redirect(high, low_code, high));                    /* past the region */
}

int main(void)
{
    struct sigaction action;

    guest = mmap(NULL, SPAN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    assert(guest != MAP_FAILED);
    low = (uint32_t)(uintptr_t)guest;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = on_fault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    assert(!sigaction(SIGSEGV, &action, NULL));

    /* xor ebx, ebx; cmp ecx, edx; mov eax, [ebx+8]; adc eax, 1; ret:
     * the load's flags are live (adc reads CF). */
    static const uint8_t load[] = { 0x31, 0xdb, 0x39, 0xd1, 0x8b, 0x43, 0x08, 0x83, 0xd0, 0x01, 0xc3 };
    compare("load", load, sizeof(load), 4, 8, 0, -1);
    /* xor ebx, ebx; cmp ecx, edx; mov [ebx+4], ecx; setc al; ret */
    static const uint8_t store[] = { 0x31, 0xdb, 0x39, 0xd1, 0x89, 0x4b, 0x04, 0x0f, 0x92, 0xc0, 0xc3 };
    compare("store", store, sizeof(store), 4, 4, 1, -1);
    /* xor ebx, ebx; lock add [ebx+0x10], ecx; ret: a read-modify-write */
    static const uint8_t locked[] = { 0x31, 0xdb, 0xf0, 0x01, 0x4b, 0x10, 0xc3 };
    compare("lock add", locked, sizeof(locked), 2, 0x10, 2, 0x044);  /* xor: ZF PF */
    /* mov esp, 0x10; push eax: esp stays 0x10 at the fault */
    static const uint8_t push[] = { 0xbc, 0x10, 0x00, 0x00, 0x00, 0x50, 0xc3 };
    compare("push", push, sizeof(push), 5, 0xc, 1, 0);
    /* mov esp, 0x20; pop eax */
    static const uint8_t pop[] = { 0xbc, 0x20, 0x00, 0x00, 0x00, 0x58, 0xc3 };
    compare("pop", pop, sizeof(pop), 5, 0x20, 0, 0);

    /* Without a fault, both modes run the program to its end alike. */
    {
        static const uint8_t fine[] = { 0x8b, 0x44, 0x24, 0x00, 0x03, 0x04, 0x24, 0xc3 };
        Run guarded = run(fine, sizeof(fine), 0), marked = run(fine, sizeof(fine), 1);
        assert(guarded.status == PW_OK && marked.status == PW_OK && !marked.redirected);
        assert(marked.state.eip == 0xdead0000u && marked.state.gpr[0] == guarded.state.gpr[0]);
    }
    test_redirect_bounds();
    printf("fault markers passed: loads, stores, a locked read-modify-write, push and pop faulting "
           "on the null page report the guard's EIP, registers, flags and fault\n");
    return 0;
}
