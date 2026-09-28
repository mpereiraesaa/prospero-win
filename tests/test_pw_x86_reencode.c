/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The same-ISA re-encoder against the emitter, on one flat guest range. */
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
#include <unistd.h>

enum { SPAN = 0x40000, CODE = 0x1000, DATA = 0x20000, STACK_TOP = 0x3f000 };

static uint8_t *guest;          /* identity-mapped: guest address == host address */
static uint32_t low, fs_offset;  /* fs base: low + fs_offset, or 0 */

static int view(void *opaque, uint32_t pc, const uint8_t **data, size_t *bytes)
{
    (void)opaque;
    if (pc < low + CODE || pc >= low + DATA) return PW_ERR_NOT_FOUND;
    *data = (const uint8_t *)(uintptr_t)pc;
    *bytes = low + DATA - pc;
    return PW_OK;
}

static void initial(PwX86State *s)
{
    memset(s, 0, sizeof(*s));
    s->eip = low + CODE;
    s->stack_low = low;
    s->stack_high = low + SPAN;
    s->memory_count = 1;
    s->memory[0].low = low;
    s->memory[0].high = (uint64_t)low + SPAN;
    s->memory[0].permissions = PW_X86_READ | PW_X86_WRITE;
    for (unsigned r = 0; r < 8; r++) s->gpr[r] = 0x11111111u * (r + 1);
    s->gpr[4] = low + STACK_TOP;
    s->gpr[6] = low + DATA;          /* esi */
    s->gpr[7] = low + DATA + 0x100;  /* edi */
    s->eflags = 0x2 | 0x040 | 0x001; /* ZF CF */
    s->fs_base = fs_offset ? low + fs_offset : 0;
}

typedef struct Run {
    PwX86State state;
    uint8_t data[0x1000];
    int status;
    uint64_t reencoded, chain_slots;
    unsigned steps;
} Run;

static unsigned unbounded;  /* pw_x86_engine_set_unbounded_chains for run() */
/* run() on a call stack (pw_x86_engine_set_call_stack): 16 KiB above a
 * 64 KiB guard, and the host faults the guard takes. */
enum { CALL_STACK_BYTES = 0x4000 };
static unsigned call_stack, call_stack_faults;
static uint8_t *call_stack_region;
static PwX86Engine *current;

static void on_fault(int sig, siginfo_t *info, void *context)
{
    ucontext_t *uc = context;
    uintptr_t rsp;

    (void)sig;
    if (!current || !pw_x86_engine_call_stack_fault(current, (uintptr_t)info->si_addr, &rsp)) {
        static const char message[] = "unexpected fault\n";
        (void)!write(2, message, sizeof(message) - 1);
        _exit(3);
    }
    uc->uc_mcontext.gregs[REG_RSP] = (greg_t)rsp;
    call_stack_faults++;
}

/* Run code from low+CODE until the guest returns to its sentinel. */
static Run run(const uint8_t *code, size_t bytes, unsigned reencode)
{
    static PwX86CacheEntry entries[512];
    static PwVmBackend vm;
    PwX86Engine engine;
    PwX86StepReport step;
    Run r;

    memset(guest + CODE, 0xcc, DATA - CODE);
    memcpy(guest + CODE, code, bytes);
    for (unsigned i = 0; i < 0x1000; i++) guest[DATA + i] = (uint8_t)(i * 7 + 3);
    initial(&r.state);
    memcpy(guest + STACK_TOP, &(uint32_t){ 0xdead0000u }, 4);
    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_engine_init(&engine, &vm, entries, 512, 1u << 20, 1, view, NULL) == PW_OK);
    assert(pw_x86_engine_set_chaining(&engine, 1) == PW_OK);
    assert(pw_x86_engine_set_indirect(&engine, 1) == PW_OK);
    assert(pw_x86_engine_set_counters(&engine, 0) == PW_OK);
    assert(pw_x86_engine_set_flat_memory(&engine, low, low + SPAN) == PW_OK);
    assert(pw_x86_engine_set_reencode(&engine, reencode) == PW_OK);
    assert(pw_x86_engine_set_unbounded_chains(&engine, unbounded || call_stack) == PW_OK);
    if (call_stack && reencode)
        assert(pw_x86_engine_set_call_stack(&engine, call_stack_region + PW_X86_ENGINE_CALL_STACK_GUARD,
                                            CALL_STACK_BYTES) == PW_OK);
    current = &engine;
    r.status = PW_OK;
    r.steps = 0;
    for (unsigned i = 0; i < 100000 && r.state.eip != 0xdead0000u; i++, r.steps++)
        if ((r.status = pw_x86_engine_step(&engine, &r.state, &step)) != PW_OK) break;
    current = NULL;
    memcpy(r.data, guest + DATA, sizeof(r.data));
    r.reencoded = engine.reencoded_blocks;
    r.chain_slots = 0;
    for (unsigned k = 0; engine.chain_targets && k < PW_X86_REENCODE_CHAIN_SLOTS; k++)
        r.chain_slots += engine.chain_targets[k].host_code != NULL;
    assert(pw_x86_engine_destroy(&engine) == PW_OK);
    return r;
}

static void same(const Run *a, const Run *b)
{
    for (unsigned g = 0; g < 8; g++) {
        if (a->state.gpr[g] != b->state.gpr[g])
            fprintf(stderr, "gpr%u %08x != %08x\n", g, a->state.gpr[g], b->state.gpr[g]);
        assert(a->state.gpr[g] == b->state.gpr[g]);
    }
    assert(a->state.eip == b->state.eip);
    if ((a->state.eflags ^ b->state.eflags) & 0x8d5)
        fprintf(stderr, "flags %03x != %03x\n", a->state.eflags & 0x8d5, b->state.eflags & 0x8d5);
    assert((a->state.eflags & 0x8d5) == (b->state.eflags & 0x8d5));
    assert(a->status == b->status);
    assert(!memcmp(a->data, b->data, sizeof(a->data)));
}

/* Run code on a call stack. */
static Run run_call_stack(const uint8_t *code, size_t bytes)
{
    Run r;
    call_stack = 1;
    r = run(code, bytes, 1);
    call_stack = 0;
    return r;
}

/* Both backends give the same result, and so does the re-encoder on a call
 * stack; the re-encoder took some blocks. */
static void compare(const uint8_t *code, size_t bytes)
{
    Run emitter = run(code, bytes, 0), reencoded = run(code, bytes, 1), stacked = run_call_stack(code, bytes);
    if (emitter.status != PW_OK || emitter.state.eip != 0xdead0000u)
        fprintf(stderr, "emitter stopped: status %d eip +%x\n", emitter.status, emitter.state.eip - low - CODE);
    assert(emitter.status == PW_OK && emitter.state.eip == 0xdead0000u);
    same(&reencoded, &emitter);
    same(&stacked, &emitter);
    assert(!emitter.reencoded && reencoded.reencoded && stacked.reencoded);
}

static void test_options(void)
{
    static const uint8_t mov[] = { 0x89, 0xc8, 0xc3 };           /* mov eax, ecx; ret */
    static const uint8_t lock[] = { 0xf0, 0x01, 0xc0, 0xc3 };    /* lock add eax, eax: #UD */
    static const uint8_t tail[] = { 0x40, 0xf3, 0xa4 };          /* inc eax; rep movsb */
    PwX86TranslateOptions o = { .flat_low = 0x10000, .flat_high = 0xfffff000u, .no_counters = 1 };
    uint8_t out[16384];
    PwX86Block block;

    assert(pw_x86_reencode(mov, sizeof(mov), 0x401000, out, sizeof(out), &block, &o) == PW_OK);
    assert(block.instructions == 2 && block.exit.kind == PW_X86_EXIT_DYNAMIC);
    assert(block.entry_contract.resident_mask == 0xff);
    assert(block.entry_contract.guest_to_host[4] == PW_X86_REENCODE_HOST_BASE + 12);
    assert(pw_x86_reencode(lock, sizeof(lock), 0x401000, out, sizeof(out), &block, &o) ==
           PW_ERR_UNSUPPORTED);
    assert(pw_x86_reencode(tail, sizeof(tail), 0x401000, out, sizeof(out), &block, &o) == PW_OK);
    assert(block.instructions == 1 && block.source_bytes == 1);
    assert(block.exit.kind == PW_X86_EXIT_DIRECT_JUMP && block.exit.target_pc == 0x401001);
    o.no_counters = 0;
    assert(pw_x86_reencode(mov, sizeof(mov), 0x401000, out, sizeof(out), &block, &o) ==
           PW_ERR_UNSUPPORTED);
    o.no_counters = 1; o.flat_high = 0;
    assert(pw_x86_reencode(mov, sizeof(mov), 0x401000, out, sizeof(out), &block, &o) ==
           PW_ERR_UNSUPPORTED);
    assert(pw_x86_reencode(NULL, 1, 0, out, sizeof(out), &block, &o) == PW_ERR_PRECONDITION);
}

/* ALU and moves on every register, esp and edi in both ModRM fields and
 * as base and index, byte registers high and low, 16-bit forms. (A form
 * that needs a REX prefix and names ah-bh, such as movzx edi, ah, ends
 * the block for the emitter instead.) */
static void test_registers(void)
{
    static const uint8_t ok[] = {
        0x89, 0xe0, 0x89, 0xfb, 0x01, 0xe7, 0x29, 0xfc, 0x01, 0xfc, 0x29, 0xe7,
        0x8b, 0x14, 0x24, 0x8b, 0x4c, 0x24, 0xfc, 0x8d, 0x6c, 0xbe, 0x10,
        0x88, 0xe1, 0x00, 0xf2, 0x66, 0x01, 0xfe, 0x66, 0x89, 0xe6,
        0x0f, 0xb6, 0xc4,                   /* movzx eax, ah */
        0x0f, 0xbf, 0xfe,                   /* movsx edi, si */
        0x0f, 0xcf,                         /* bswap edi */
        0x4c, 0x44, 0x47, 0x4f,             /* dec esp, inc esp, inc edi, dec edi */
        0xbf, 0x78, 0x56, 0x34, 0x12,       /* mov edi, 0x12345678 */
        0xc1, 0xe7, 0x05,                   /* shl edi, 5 */
        0xd3, 0xcf,                         /* ror edi, cl */
        0x0f, 0xaf, 0xfa,                   /* imul edi, edx */
        0x6b, 0xc7, 0x13,                   /* imul eax, edi, 19 */
        0xf7, 0xe7,                         /* mul edi */
        0x99,                               /* cdq */
        0x39, 0xd8,                         /* cmp eax, ebx: every flag defined */
        0xc3,
    };
    compare(ok, sizeof(ok));
}

/* xchg between registers, which only the re-encoder takes. */
static void test_xchg(void)
{
    static const uint8_t code[] = {
        0x87, 0xe7, 0x87, 0xe7,             /* xchg edi, esp, twice */
        0x87, 0xfb,                         /* xchg ebx, edi */
        0x97,                               /* xchg eax, edi */
        0x86, 0xe1,                         /* xchg cl, ah */
        0xc3,
    };
    Run r = run(code, sizeof(code), 1);
    assert(r.status == PW_OK && r.state.eip == 0xdead0000u);
    assert(r.state.gpr[3] == low + DATA + 0x100 && r.state.gpr[0] == 0x44442244u);
    assert(r.state.gpr[7] == 0x11111111u && r.state.gpr[1] == 0x22222244u);
    assert(r.state.gpr[4] == low + STACK_TOP + 4);
}

static uint32_t pattern(uint32_t offset)
{
    uint32_t v = 0;
    for (unsigned k = 0; k < 4; k++) v |= (uint32_t)(uint8_t)((offset + k) * 7 + 3) << (8 * k);
    return v;
}

/* Atomic read-modify-writes with lock, xchg, cmpxchg, xadd and cmpxchg8b
 * on memory, and fs and the flat segment overrides. */
static void test_atomic_and_segments(void)
{
    static const uint8_t code[] = {
        0xf0, 0x01, 0x06,                   /* lock add [esi], eax */
        0xf0, 0x0f, 0xc1, 0x4e, 0x04,       /* lock xadd [esi+4], ecx */
        0x87, 0x56, 0x08,                   /* xchg [esi+8], edx */
        0x8b, 0x46, 0x0c,                   /* mov eax, [esi+12] */
        0xf0, 0x0f, 0xb1, 0x5e, 0x0c,       /* lock cmpxchg [esi+12], ebx: equal */
        0xf0, 0x0f, 0xc7, 0x4e, 0x10,       /* lock cmpxchg8b [esi+16]: not equal */
        0x64, 0xa1, 0, 0, 0, 0,             /* mov eax, fs:[0] */
        0x64, 0x89, 0x0d, 4, 0, 0, 0,       /* mov fs:[4], ecx */
        0x2e, 0x8b, 0x5e, 0x20,             /* mov ebx, cs:[esi+0x20] */
        0xc3,
    };
    const uint32_t *data;
    Run r;

    fs_offset = DATA + 0x200;
    r = run(code, sizeof(code), 1);
    fs_offset = 0;
    data = (const uint32_t *)(const void *)r.data;
    assert(r.status == PW_OK && r.state.eip == 0xdead0000u);
    assert(data[0] == pattern(0) + 0x11111111u);
    assert(data[1] == pattern(4) + 0x22222222u);
    assert(data[2] == 0x33333333u && data[3] == 0x44444444u);
    assert(data[4] == pattern(16) && data[5] == pattern(20));
    assert(data[0x204 / 4] == pattern(4));
    assert(r.state.gpr[0] == pattern(0x200) && r.state.gpr[1] == pattern(4));
    assert(r.state.gpr[2] == pattern(20) && r.state.gpr[3] == pattern(0x20));
    assert(!(r.state.eflags & 0x040));
    assert(r.reencoded);
}

/* Memory operands through the guard: loads, stores, read-modify-write,
 * immediates, moffs, setcc and cmov with memory. */
static void test_memory(void)
{
    uint8_t code[] = {
        0xb9, 0x04, 0x00, 0x00, 0x00,       /* mov ecx, 4 */
        0x8b, 0x06,                         /* mov eax, [esi] */
        0x03, 0x47, 0x04,                   /* add eax, [edi+4] */
        0x89, 0x47, 0x08,                   /* mov [edi+8], eax */
        0x01, 0x07,                         /* add [edi], eax */
        0x83, 0x6f, 0x0c, 0x05,             /* sub dword [edi+12], 5 */
        0x80, 0x76, 0x10, 0x5a,             /* xor byte [esi+16], 0x5a */
        0xc7, 0x44, 0x8e, 0x20, 1, 2, 3, 4, /* mov dword [esi+ecx*4+0x20], 0x04030201 */
        0x66, 0xc7, 0x47, 0x30, 0x34, 0x12, /* mov word [edi+0x30], 0x1234 */
        0xa1, 0, 0, 0, 0,                   /* mov eax, [moffs] (patched) */
        0xa3, 0, 0, 0, 0,                   /* mov [moffs+4], eax (patched) */
        0x39, 0x06,                         /* cmp [esi], eax */
        0x0f, 0x94, 0xc2,                   /* setz dl */
        0x88, 0x57, 0x40,                   /* mov [edi+0x40], dl */
        0x0f, 0x42, 0x5e, 0x44,             /* cmovb ebx, [esi+0x44] */
        0x13, 0x4e, 0x48,                   /* adc ecx, [esi+0x48] */
        0xd1, 0x67, 0x50,                   /* shl dword [edi+0x50], 1 */
        0xf7, 0x5f, 0x54,                   /* neg dword [edi+0x54] */
        0x0f, 0xb6, 0x56, 0x58,             /* movzx edx, byte [esi+0x58] */
        0x0f, 0xbf, 0x6e, 0x5a,             /* movsx ebp, word [esi+0x5a] */
        0xff, 0x47, 0x60,                   /* inc dword [edi+0x60] */
        0x85, 0x46, 0x64,                   /* test [esi+0x64], eax */
        0x39, 0xd8,                         /* cmp eax, ebx: test leaves AF undefined */
        0xc3,
    };
    uint32_t moffs = low + DATA + 0x80, moffs4 = moffs + 4;
    memcpy(code + 38, &moffs, 4);
    memcpy(code + 43, &moffs4, 4);
    compare(code, sizeof(code));
}

/* Flags across linked blocks (a compare in one block, the branch in the
 * next), a loop, calls, returns, push and pop of every kind, and leave. */
static void test_control(void)
{
    static const uint8_t code[] = {
        0x55,                               /* 00 push ebp */
        0x89, 0xe5,                         /* 01 mov ebp, esp */
        0x57, 0x56,                         /* 03 push edi; push esi */
        0xb9, 0x20, 0, 0, 0,                /* 05 mov ecx, 32 */
        0x31, 0xc0,                         /* 0a xor eax, eax */
        0x01, 0xc8,                         /* 0c L: add eax, ecx */
        0x39, 0xc1,                         /* 0e cmp ecx, eax */
        0xeb, 0x00,                         /* 10 jmp +0 (the flags cross a link) */
        0x72, 0x01,                         /* 12 jb +1 */
        0x40,                               /* 14 inc eax */
        0x49,                               /* 15 dec ecx */
        0x75, 0xf4,                         /* 16 jnz L */
        0xe8, 0x0d, 0, 0, 0,                /* 18 call F */
        0x6a, 0xfe,                         /* 1d push -2 */
        0x68, 0x44, 0x33, 0x22, 0x11,       /* 1f push 0x11223344 */
        0x5b, 0x5a,                         /* 24 pop ebx; pop edx */
        0x5e, 0x5f,                         /* 26 pop esi; pop edi */
        0xc9,                               /* 28 leave */
        0xc3,                               /* 29 ret */
        0xff, 0x36,                         /* 2a F: push dword [esi] */
        0x5f,                               /* 2c pop edi */
        0x54,                               /* 2d push esp */
        0x58,                               /* 2e pop eax */
        0xc2, 0x00, 0x00,                   /* 2f ret 0 */
    };
    compare(code, sizeof(code));
}

/* A block the re-encoder stops in the middle (div, which it leaves to the
 * emitter) and indirect calls and jumps through registers and memory. */
static void test_mixed_and_indirect(void)
{
    uint8_t code[] = {
        0xb8, 100, 0, 0, 0,                 /* 00 mov eax, 100 */
        0x31, 0xd2,                         /* 05 xor edx, edx */
        0xb9, 7, 0, 0, 0,                   /* 07 mov ecx, 7 */
        0xf7, 0xf1,                         /* 0c div ecx (emitter) */
        0x8d, 0x1c, 0x10,                   /* 0e lea ebx, [eax+edx] */
        0xbf, 0, 0, 0, 0,                   /* 11 mov edi, F (patched) */
        0xff, 0xd7,                         /* 16 call edi */
        0x89, 0x3e,                         /* 18 mov [esi], edi */
        0xff, 0x16,                         /* 1a call [esi] */
        0x39, 0xd8,                         /* 1c cmp eax, ebx: div left the flags undefined */
        0xc3,                               /* 1e ret */
        0x43,                               /* 1f F: inc ebx */
        0xc3,                               /* 20 ret */
    };
    uint32_t f = low + CODE + 0x1f;
    memcpy(code + 0x12, &f, 4);
    compare(code, sizeof(code));
}

/* Returns and indirect calls between re-encoded blocks enter their target's
 * chain entry with the guest state still pinned once the target is known
 * (the dispatcher records it in the chain table). */
static void test_return_targets(void)
{
    uint8_t code[] = {
        0xb9, 0xc8, 0, 0, 0,                /* 00 mov ecx, 200 */
        0x31, 0xc0,                         /* 05 xor eax, eax */
        0xbb, 0, 0, 0, 0,                   /* 07 mov ebx, G (patched) */
        0xe8, 0x0b, 0, 0, 0,                /* 0c L: call F */
        0xff, 0xd3,                         /* 11 call ebx */
        0x49,                               /* 13 dec ecx */
        0x75, 0xf6,                         /* 14 jnz L */
        0x39, 0xd8,                         /* 16 cmp eax, ebx */
        0xc3,                               /* 18 ret */
        0x00, 0x00, 0x00,                   /* 19 padding */
        0x01, 0xc8,                         /* 1c F: add eax, ecx */
        0xc3,                               /* 1e ret */
        0x40,                               /* 1f G: inc eax */
        0xc3,                               /* 20 ret */
    };
    uint32_t g = low + CODE + 0x1f;
    Run emitter, reencoded;
    memcpy(code + 8, &g, 4);
    emitter = run(code, sizeof(code), 0);
    reencoded = run(code, sizeof(code), 1);
    same(&reencoded, &emitter);
    reencoded = run_call_stack(code, sizeof(code));
    same(&reencoded, &emitter);
    assert(reencoded.state.gpr[0] == 200u * 201u / 2u + 200u);
    assert(!emitter.chain_slots && reencoded.chain_slots >= 3);
}

/* A refused access stops with every register and, since a branch reads
 * them later, the flags of the compare before it. */
/* A loop with a call and a return in it: unbounded chains give the same
 * result without returning to the dispatcher once linked, where bounded
 * ones return whenever the budget runs out. */
static void test_unbounded_chains(void)
{
    static const uint8_t code[] = {
        0xb9, 0xb8, 0x0b, 0, 0,             /* 00 mov ecx, 3000 */
        0x31, 0xc0,                         /* 05 xor eax, eax */
        0xe8, 0x04, 0, 0, 0,                /* 07 L: call F */
        0x49,                               /* 0c dec ecx */
        0x75, 0xf8,                         /* 0d jnz L */
        0xc3,                               /* 0f ret */
        0x01, 0xc8,                         /* 10 F: add eax, ecx */
        0xc3,                               /* 12 ret */
    };
    Run emitter = run(code, sizeof(code), 0), bounded, free_running;

    bounded = run(code, sizeof(code), 1);
    unbounded = 1;
    free_running = run(code, sizeof(code), 1);
    unbounded = 0;
    assert(emitter.status == PW_OK && emitter.state.eip == 0xdead0000u);
    assert(emitter.state.gpr[0] == 3000u * 3001u / 2);
    same(&bounded, &emitter);
    same(&free_running, &emitter);
    free_running = run_call_stack(code, sizeof(code));
    same(&free_running, &emitter);
    assert(free_running.steps <= 16);
    /* 6000 charged exits at the default quantum of 64 against a handful of
     * steps to translate and link the four blocks. */
    if (free_running.steps > 16 || bounded.steps < 6000 / PW_X86_ENGINE_DEFAULT_QUANTUM)
        fprintf(stderr, "unbounded chains: %u steps bounded, %u unbounded\n", bounded.steps, free_running.steps);
    assert(free_running.steps <= 16 && bounded.steps >= 6000 / PW_X86_ENGINE_DEFAULT_QUANTUM);
}

/* Returns the call stack does not predict: a push/ret trampoline, a call
 * whose return address is popped (the PC thunk), ret imm16 over arguments,
 * and recursion deeper than the call stack, which runs out into the guard
 * and starts it again. The re-encoder on a call stack matches the emitter. */
static void test_call_stack(void)
{
    uint8_t code[] = {
        0x68, 0, 0, 0, 0,                   /* 00 push T (patched) */
        0xc3,                               /* 05 ret: to T */
        0xcc,                               /* 06 */
        0xe8, 0, 0, 0, 0,                   /* 07 T: call P */
        0x58,                               /* 0c P: pop eax (the PC thunk) */
        0x6a, 0x05,                         /* 0d push 5 */
        0x6a, 0x07,                         /* 0f push 7 */
        0xe8, 0x12, 0, 0, 0,                /* 11 call A */
        0xb9, 0xb8, 0x0b, 0, 0,             /* 16 mov ecx, 3000 */
        0xe8, 0x13, 0, 0, 0,                /* 1b call R */
        0x01, 0xd8,                         /* 20 add eax, ebx */
        0xc3,                               /* 22 ret */
        0xcc, 0xcc, 0xcc, 0xcc, 0xcc,       /* 23 */
        0x8b, 0x5c, 0x24, 0x04,             /* 28 A: mov ebx, [esp+4] */
        0x03, 0x5c, 0x24, 0x08,             /* 2c add ebx, [esp+8] */
        0xc2, 0x08, 0x00,                   /* 30 ret 8 */
        0x49,                               /* 33 R: dec ecx */
        0x74, 0x06,                         /* 34 jz done */
        0xe8, 0xf8, 0xff, 0xff, 0xff,       /* 36 call R */
        0x43,                               /* 3b inc ebx */
        0xc3,                               /* 3c done: ret */
    };
    const uint32_t t = low + CODE + 7;
    Run emitter, stacked;

    memcpy(code + 1, &t, 4);
    emitter = run(code, sizeof(code), 0);
    call_stack_faults = 0;
    stacked = run_call_stack(code, sizeof(code));
    assert(emitter.status == PW_OK && emitter.state.eip == 0xdead0000u);
    same(&stacked, &emitter);
    assert(emitter.state.gpr[3] == 12 + 2999 && emitter.state.gpr[1] == 0);
    /* 3000 nested calls take 24000 bytes of a 16 KiB call stack. */
    assert(call_stack_faults >= 1);
}

static void test_fault(void)
{
    static const uint8_t code[] = {
        0xb8, 5, 0, 0, 0,                   /* 00 mov eax, 5 */
        0x8d, 0x3c, 0x00,                   /* 05 lea edi, [eax+eax] */
        0x39, 0xf8,                         /* 08 cmp eax, edi: CF SF */
        0x89, 0x05, 0x10, 0, 0, 0,          /* 0a mov [0x10], eax: outside */
        0x72, 0x00,                         /* 10 jb */
        0xc3,
    };
    Run emitter = run(code, sizeof(code), 0), reencoded = run(code, sizeof(code), 1);
    assert(reencoded.status != PW_OK && reencoded.state.eip == low + CODE + 0x0a);
    assert(reencoded.state.gpr[0] == 5 && reencoded.state.gpr[7] == 10);
    assert((reencoded.state.eflags & 0x8d5) == 0x091);          /* SF AF CF */
    same(&reencoded, &emitter);
}

int main(void)
{
    guest = mmap(NULL, SPAN, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    assert(guest != MAP_FAILED);
    low = (uint32_t)(uintptr_t)guest;
    call_stack_region = mmap(NULL, PW_X86_ENGINE_CALL_STACK_GUARD + CALL_STACK_BYTES, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(call_stack_region != MAP_FAILED);
    assert(!mprotect(call_stack_region, PW_X86_ENGINE_CALL_STACK_GUARD, PROT_NONE));
    {
        struct sigaction action;
        static uint8_t alternate[1 << 16];
        stack_t stack = { .ss_sp = alternate, .ss_size = sizeof(alternate) };
        memset(&action, 0, sizeof(action));
        action.sa_sigaction = on_fault;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        assert(!sigaltstack(&stack, NULL));
        assert(!sigaction(SIGSEGV, &action, NULL));
    }
    test_options();
    test_registers();
    test_xchg();
    test_atomic_and_segments();
    test_memory();
    test_control();
    test_mixed_and_indirect();
    test_return_targets();
    test_unbounded_chains();
    test_call_stack();
    test_fault();
    printf("reencode passed: options, register remapping, xchg, atomics and segments, memory operands, flags across links, "
           "stack and calls, emitter hand-over, indirect targets, pinned returns, unbounded chains, call stack, fault state\n");
    return 0;
}
