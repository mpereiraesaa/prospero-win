/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The same-ISA re-encoder against the emitter, on one flat guest range. */
#define _GNU_SOURCE
#include "../src/pw_x86_engine.h"
#include "../src/pw_x86_reencode.h"
#include "../src/pw_guest_fp.h"
#include "../src/pw_x86_hostexec.h"
#include "../src/pw_vm_posix.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

enum { SPAN = 0x40000, CODE = 0x1000, DATA = 0x20000, STACK_TOP = 0x3f000 };

static uint8_t *guest;          /* identity-mapped: guest address == host address */
static uint32_t low, fs_offset;  /* fs base: low + fs_offset, or 0 */
static uint32_t source_pc;       /* optional synthetic code PC, data stays mapped */

static int view(void *opaque, uint32_t pc, const uint8_t **data, size_t *bytes)
{
    (void)opaque;
    if (source_pc) {
        const uint32_t offset = pc - source_pc;
        if (offset >= DATA - CODE) return PW_ERR_NOT_FOUND;
        *data = guest + CODE + offset;
        *bytes = DATA - CODE - offset;
        return PW_OK;
    }
    if (pc < low + CODE || pc >= low + DATA) return PW_ERR_NOT_FOUND;
    *data = (const uint8_t *)(uintptr_t)pc;
    *bytes = low + DATA - pc;
    return PW_OK;
}

static void initial(PwX86State *s)
{
    memset(s, 0, sizeof(*s));
    s->eip = source_pc ? source_pc : low + CODE;
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
    pw_guest_fp_init(&s->fp);  /* exceptions masked, as a Windows thread starts */
}

typedef struct Run {
    PwX86State state;
    uint8_t data[0x1000];
    int status;
    uint64_t reencoded, chain_slots;
    unsigned steps;
} Run;

static unsigned unbounded;  /* pw_x86_engine_set_unbounded_chains for run() */
static unsigned native_fp;  /* pw_x86_engine_set_native_fp for run()'s re-encoder */
/* run() executes what neither translator takes on the host, one instruction
 * at a time, as wowprospero does. */
static unsigned hostexec_fallback;
static PwX86HostExec hostexec;
/* run() on a call stack (pw_x86_engine_set_call_stack): 16 KiB above a
 * 64 KiB guard, and the host faults the guard takes. */
enum { CALL_STACK_BYTES = 0x4000 };
static unsigned call_stack, call_stack_faults;
/* run() with superblocks, whose side exits rewrite their own code: the
 * engine's code stays writable (and executable) while it runs. */
static unsigned superblocks;
/* run() with pw_x86_engine_set_call_predict, which needs superblocks' writable code. */
static unsigned call_predict;
static PwVmBackend posix;

static int writable_commit(void *context, const PwVmRegion *region, size_t offset, size_t bytes, unsigned protection)
{
    (void)protection;
    return posix.commit(context, region, offset, bytes, PW_PROT_READ | PW_PROT_WRITE | PW_PROT_EXEC);
}

static int writable_protect(void *context, const PwVmRegion *region, size_t offset, size_t bytes, unsigned protection)
{
    (void)protection;
    return posix.protect(context, region, offset, bytes, PW_PROT_READ | PW_PROT_WRITE | PW_PROT_EXEC);
}
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
    if (superblocks) {
        posix = vm;
        vm.commit = writable_commit;
        vm.protect = writable_protect;
    }
    assert(pw_x86_engine_init(&engine, &vm, entries, 512, 1u << 20, 1, view, NULL) == PW_OK);
    assert(pw_x86_engine_set_chaining(&engine, 1) == PW_OK);
    assert(pw_x86_engine_set_indirect(&engine, 1) == PW_OK);
    assert(pw_x86_engine_set_counters(&engine, 0) == PW_OK);
    assert(pw_x86_engine_set_flat_memory(&engine, low, low + SPAN) == PW_OK);
    assert(pw_x86_engine_set_reencode(&engine, reencode) == PW_OK);
    assert(pw_x86_engine_set_unbounded_chains(&engine, unbounded || call_stack || superblocks) == PW_OK);
    assert(pw_x86_engine_set_superblocks(&engine, superblocks) == PW_OK);
    assert(pw_x86_engine_set_call_predict(&engine, call_predict && superblocks) == PW_OK);
    assert(pw_x86_engine_set_native_fp(&engine, native_fp && reencode) == PW_OK);
    if (call_stack && reencode)
        assert(pw_x86_engine_set_call_stack(&engine, call_stack_region + PW_X86_ENGINE_CALL_STACK_GUARD,
                                            CALL_STACK_BYTES) == PW_OK);
    current = &engine;
    r.status = PW_OK;
    r.steps = 0;
    for (unsigned i = 0; i < 100000 && r.state.eip != 0xdead0000u; i++, r.steps++) {
        r.status = pw_x86_engine_step(&engine, &r.state, &step);
        if (r.status == PW_ERR_UNSUPPORTED && hostexec_fallback) {
            /* As wowprospero does: the one instruction on the host. */
            const uint8_t *at = (const uint8_t *)(uintptr_t)r.state.eip;
            pw_x86_commit_canonical_flags(&r.state);
            pw_x86_engine_fp_sync(&engine, &r.state);
            if ((r.status = pw_x86_hostexec_step(&hostexec, &r.state, at, 15)) == PW_OK) continue;
        }
        if (r.status != PW_OK) break;
    }
    current = NULL;
    pw_x86_engine_fp_sync(&engine, &r.state);
    memcpy(r.data, guest + DATA, sizeof(r.data));
    r.reencoded = engine.reencoded_blocks;
    r.chain_slots = 0;
    for (unsigned k = 0; engine.chain_targets && k < PW_X86_REENCODE_CHAIN_ENTRIES; k++)
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

/* Run code with superblocks and a call stack. */
static Run run_superblocks(const uint8_t *code, size_t bytes)
{
    Run r;
    superblocks = 1;
    r = run_call_stack(code, bytes);
    superblocks = 0;
    return r;
}

/* Both backends give the same result, and so does the re-encoder on a call
 * stack and with superblocks; the re-encoder took some blocks. */
static void compare(const uint8_t *code, size_t bytes)
{
    Run emitter = run(code, bytes, 0), reencoded = run(code, bytes, 1), stacked = run_call_stack(code, bytes);
    Run super = run_superblocks(code, bytes);
    same(&super, &emitter);
    assert(super.reencoded);
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
    static const uint8_t tail[] = { 0x40, 0xf7, 0xf1 };          /* inc eax; div ecx */
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

/* MPX's bnd prefix (F2) on jumps, calls and returns, as FFmpeg's assembly
 * has it (LAV Filters): a processor without MPX ignores it. */
static void test_bnd_branches(void)
{
    static const uint8_t code[] = {
        0xb9, 0x04, 0, 0, 0,                /* 00 mov ecx, 4 */
        0x31, 0xc0,                         /* 05 xor eax, eax */
        0x40,                               /* 07 L: inc eax */
        0x49,                               /* 08 dec ecx */
        0xf2, 0x75, 0xfb,                   /* 09 bnd jnz L */
        0xf2, 0xe8, 0x12, 0, 0, 0,          /* 0c bnd call F */
        0x85, 0xc0,                         /* 12 test eax, eax */
        0xf2, 0x0f, 0x85, 0x01, 0, 0, 0,    /* 14 bnd jnz near +1 */
        0x40,                               /* 1b inc eax */
        0xf2, 0xe9, 0x00, 0, 0, 0,          /* 1c bnd jmp +0 */
        0xf2, 0xc3,                         /* 22 bnd ret */
        0x01, 0xc0,                         /* 24 F: add eax, eax */
        0xf2, 0xc3,                         /* 26 bnd ret */
    };
    /* The same without the prefix. */
    static const uint8_t plain[] = {
        0xb9, 0x04, 0, 0, 0,                /* 00 mov ecx, 4 */
        0x31, 0xc0,                         /* 05 xor eax, eax */
        0x40,                               /* 07 L: inc eax */
        0x49,                               /* 08 dec ecx */
        0x75, 0xfc,                         /* 09 jnz L */
        0xe8, 0x0f, 0, 0, 0,                /* 0b call F */
        0x85, 0xc0,                         /* 10 test eax, eax */
        0x0f, 0x85, 0x01, 0, 0, 0,          /* 12 jnz near +1 */
        0x40,                               /* 18 inc eax */
        0xe9, 0x00, 0, 0, 0,                /* 19 jmp +0 */
        0xc3,                               /* 1e ret */
        0x01, 0xc0,                         /* 1f F: add eax, eax */
        0xc3,                               /* 21 ret */
    };
    Run bnd = run(code, sizeof(code), 0), without = run(plain, sizeof(plain), 0);

    compare(code, sizeof(code));
    assert(bnd.state.gpr[0] == 8 && without.state.gpr[0] == 8);
    same(&bnd, &without);
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

/* Invalid null targets return to the dispatcher as the emitter does; they
 * must not jump to host NULL, including a ret through the empty call stack. */
static void test_null_targets(void)
{
    static uint8_t sources[][8] = {
        {0xb8,0,0,0,0,0xff,0xd0},       /* mov eax,0; call eax */
        {0xb8,0,0,0,0,0xff,0xe0},       /* mov eax,0; jmp eax */
        {0xc7,0x06,0,0,0,0,0xff,0x16},  /* mov dword [esi],0; call [esi] */
        {0xc7,0x06,0,0,0,0,0xff,0x26},  /* mov dword [esi],0; jmp [esi] */
        {0xc7,0x04,0x24,0,0,0,0,0xc3}, /* mov dword [esp],0; ret */
        {0x0f,0x84,0,0,0,0,0x90,0xc3}, /* jz 0; nop; ret (side exit) */
    };
    static const size_t bytes[] = {7,7,8,8,8,8};
    /* The caller must not occupy slot zero itself: a randomized low mapping
     * can otherwise hide the empty-slot bug by publishing a nonzero tag. */
    source_pc = 0x12341000;
    const uint32_t relative = 0u - (source_pc + 6);
    memcpy(sources[5] + 2, &relative, sizeof(relative));
    for (unsigned k = 0; k < sizeof(bytes) / sizeof(bytes[0]); k++) {
        Run reference = run(sources[k], bytes[k], 0);
        Run ordinary = run(sources[k], bytes[k], 1);
        Run stacked = run_call_stack(sources[k], bytes[k]);
        Run super = run_superblocks(sources[k], bytes[k]);
        assert(reference.status == PW_ERR_NOT_FOUND && reference.state.eip == 0);
        same(&reference, &ordinary);
        same(&reference, &stacked);
        same(&reference, &super);
        unbounded = 1;
        ordinary = run(sources[k], bytes[k], 1);
        unbounded = 0;
        same(&reference, &ordinary);
    }
    source_pc = 0;
}

/* Returns and indirect calls between re-encoded blocks enter their target's
 * chain entry with the guest state still pinned once the target is known
 * (the dispatcher records it in the chain table). */
/* Two colliding callees must retain both entries and incoming flags. */
static void test_two_way_calls(void)
{
    uint8_t *code=calloc(1, DATA-CODE);
    uint8_t caller[]={
        0xb9,64,0,0,0, 0xbb,0,0,0,0, 0xb8,0,0,0,0,
        0x39,0xc9,                         /* L: cmp ecx,ecx: clear CF */
        0xff,0xd3,                         /* call ebx */
        0x81,0xf3,0,0,0,0,                /* xor ebx,F^G */
        0x49,0x75,0xf3,                    /* dec ecx; jnz L */
        0x3d,128,0,0,0, 0xc3,
    };
    const uint8_t fcode[]={0x83,0xd0,1,0xc3}, gcode[]={0x83,0xd0,3,0xc3};
    uint32_t f=low+CODE+0x100, g=0, mask;
    Run captured;
    assert(code);
    for(uint32_t candidate=f+16; candidate<low+DATA-16; candidate++)
        if((candidate & 0xffffu)==(f & 0xffffu)) {g=candidate;break;}
    assert(g && g!=f);
    mask=f^g;
    memcpy(caller+6,&f,4);memcpy(caller+21,&mask,4);
    memcpy(code,caller,sizeof(caller));
    memcpy(code+f-low-CODE,fcode,sizeof(fcode));
    memcpy(code+g-low-CODE,gcode,sizeof(gcode));
    compare(code,DATA-CODE);
    unbounded=1;captured=run(code,DATA-CODE,1);unbounded=0;
    assert(captured.status==PW_OK && captured.state.eip==0xdead0000u);
    assert(captured.state.gpr[0]==128 && captured.state.gpr[1]==0);
    assert(captured.state.eflags & 0x40);
    assert(captured.steps<12); /* Cold publication, then both banks chain. */
    free(code);
}

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

/* A predicted call site (PwX86TranslateOptions.call_predict), run as one
 * block against a chain table. call edx is looked up until the table finds
 * its target, then jumps to that first target directly: with the target's
 * entry missing from the table the call still reaches it. Another target
 * keeps the lookup without replacing the first, and a missing one returns
 * to the dispatcher. Untrained, target 0 is looked up and trains nothing.
 * The callee's SETcc observe the caller's flags on every path. */
static void test_call_predict_site(void)
{
    static const unsigned flags[] = { 0, 0x8d5, 0x41, 0x894 };
    static const uint8_t caller[] = { 0xff, 0xd2 }; /* call edx */
    static const uint8_t destination[] = {
        0x0f, 0x92, 0xc0, /* setb al */
        0x0f, 0x94, 0xc3, /* sete bl */
        0x0f, 0x9a, 0xc2, /* setp dl */
        0x0f, 0x90, 0xc4, /* seto ah */
        0x0f, 0x98, 0xc7, /* sets bh */
    };
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    const size_t table_bytes = PW_X86_REENCODE_CHAIN_ENTRIES * sizeof(PwX86IndirectTarget);
    uint8_t *region = mmap(NULL, table_bytes + 2 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t *code = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    const uint32_t pc = low + CODE, a = low + CODE + 0x100, other = low + CODE + 0x200;
    const uint32_t ends[2] = { a + (uint32_t)sizeof(destination), other + (uint32_t)sizeof(destination) };
    PwX86IndirectTarget indirect = {0}, *table, *slot_a, *slot_other;
    PwX86TranslateOptions options = {
        .flat_low = low, .flat_high = low + SPAN, .no_counters = 1, .indirect_targets = &indirect,
        .native_fp = 1, .unbounded_chains = 1, .call_stack = 1, .call_predict = 1,
    };
    PwX86Block block, callee;

    assert(region != MAP_FAILED && code != MAP_FAILED);
    assert(!mprotect(region + page, table_bytes, PROT_READ | PROT_WRITE));
    table = (PwX86IndirectTarget *)(region + page);
    options.chain_targets = table;
    /* The empty slot 0 must not match target 0 (its entry has no code). */
    table[0].guest_pc = table[PW_X86_REENCODE_CHAIN_SLOTS].guest_pc = 1;
    slot_a = table + (a & 0xffffu);          /* the chain table index */
    slot_other = table + (other & 0xffffu);
    assert(slot_a != slot_other && slot_a != table && slot_other != table);
    assert(pw_x86_reencode(destination, sizeof(destination), a, code + page, page, &callee, &options) == PW_OK);
    slot_a->host_code = code + page + callee.chain_entry_offset;
    assert(pw_x86_reencode(destination, sizeof(destination), other, code + 2 * page, page, &callee, &options) == PW_OK);
    slot_other->host_code = code + 2 * page + callee.chain_entry_offset;

    /* Each step: the target, whether its slot holds it, where the run ends
     * (0: at the target, in the dispatcher; 1/2: past a or other's code). */
    static const struct { unsigned target; unsigned listed; unsigned reached; } steps[] = {
        { 0, 0, 0 },    /* a missing: looked up, missed */
        { 0, 1, 1 },    /* a listed: looked up, trains */
        { 0, 0, 1 },    /* a missing: predicted */
        { 1, 1, 2 },    /* other listed: looked up */
        { 0, 0, 1 },    /* a missing: still predicted */
        { 1, 0, 0 },    /* other missing: looked up, missed */
    };
    for (unsigned f = 0; f < sizeof(flags) / sizeof(flags[0]); f++) {
        assert(pw_x86_reencode(caller, sizeof(caller), pc, code, page, &block, &options) == PW_OK);
        for (unsigned k = 0; k < sizeof(steps) / sizeof(steps[0]); k++) {
            const uint32_t target = steps[k].target ? other : a;
            PwX86IndirectTarget *slot = steps[k].target ? slot_other : slot_a;
            PwX86State state;
            uint32_t expected[8];
            slot->guest_pc = steps[k].listed ? target : target ^ 1u;
            initial(&state);
            state.gpr[2] = target;
            state.eflags = 2 | flags[f];
            state.chain_budget = 8;
            state.call_stack_top = (uintptr_t)call_stack_region + PW_X86_ENGINE_CALL_STACK_GUARD + CALL_STACK_BYTES;
            memcpy(expected, state.gpr, sizeof(expected));
            expected[4] -= 4;
            if (steps[k].reached) {
                expected[0] = (expected[0] & 0xffff0000u) |
                    ((flags[f] & 0x800) ? 0x100u : 0u) | (flags[f] & 1);
                expected[3] = (expected[3] & 0xffff0000u) |
                    ((flags[f] & 0x80) ? 0x100u : 0u) | ((flags[f] & 0x40) != 0);
                expected[2] = (expected[2] & 0xffffff00u) | ((flags[f] & 4) != 0);
            }
            assert(pw_x86_run_block(&state, code) == 0);
            if (state.eip != (steps[k].reached ? ends[steps[k].reached - 1] : target))
                fprintf(stderr, "call predict: flags %03x step %u eip %08x\n", flags[f], k, state.eip);
            assert(state.eip == (steps[k].reached ? ends[steps[k].reached - 1] : target));
            assert(!memcmp(state.gpr, expected, sizeof(expected)));
            assert((state.eflags & 0x8d5) == flags[f]);
            {
                uint32_t pushed;
                memcpy(&pushed, guest + STACK_TOP - 4, 4);
                assert(pushed == pc + (uint32_t)sizeof(caller));
            }
        }
        slot_a->guest_pc = a;
        slot_other->guest_pc = other;
    }
    /* Untrained, target 0 goes to the lookup, which misses, and trains
     * nothing: a afterwards, missing, is looked up too. */
    assert(pw_x86_reencode(caller, sizeof(caller), pc, code, page, &block, &options) == PW_OK);
    for (unsigned k = 0; k < 2; k++) {
        PwX86State state;
        initial(&state);
        state.gpr[2] = k ? a : 0;
        state.chain_budget = 8;
        state.call_stack_top = (uintptr_t)call_stack_region + PW_X86_ENGINE_CALL_STACK_GUARD + CALL_STACK_BYTES;
        slot_a->guest_pc = a ^ 1u;
        assert(pw_x86_run_block(&state, code) == 0);
        assert(state.eip == (k ? a : 0u));
    }
    assert(!munmap(code, 3 * page));
    assert(!munmap(region, table_bytes + 2 * page));
}

/* Predicted calls in the engine, whose code learns its targets in place: a
 * monomorphic call through memory, a site alternating between two
 * targets, a callee reading the CF its caller set, and a callee that
 * returns past its call (the return's own lookup), a hundred times each.
 * The result matches the emitter's and the re-encoder's without
 * prediction. */
static void test_call_predict_engine(void)
{
    enum { F1 = 0x80, F2 = 0x90, F3 = 0xa0, F4 = 0xb0, TABLE = 0x30000 };
    static const uint8_t program[] = {
        0xb8, 0, 0, 0, 0,                   /* 00 mov eax, table */
        0xb9, 100, 0, 0, 0,                 /* 05 mov ecx, 100 */
        0x31, 0xf6,                         /* 0a xor esi, esi */
        0xff, 0x50, 0x04,                   /* 0c loop: call [eax+4]: f1 */
        0xff, 0x14, 0xb0,                   /* 0f call [eax+esi*4]: f2, f1, ... */
        0x83, 0xf6, 0x01,                   /* 12 xor esi, 1 */
        0x83, 0xf9, 0x32,                   /* 15 cmp ecx, 50 */
        0xff, 0x50, 0x08,                   /* 18 call [eax+8]: f3, adc */
        0xff, 0x50, 0x0c,                   /* 1b call [eax+0xc]: f4, returns to 21 */
        0xcc, 0xcc, 0xcc,                   /* 1e */
        0x49,                               /* 21 dec ecx */
        0x75, 0xe8,                         /* 22 jnz loop */
        0xc3,                               /* 24 ret */
    };
    static const uint8_t f1[] = { 0x83, 0xc3, 0x01, 0xc3 };             /* add ebx, 1 */
    static const uint8_t f2[] = { 0x83, 0xc7, 0x03, 0xc3 };             /* add edi, 3 */
    static const uint8_t f3[] = { 0x83, 0xd5, 0x00, 0xc3 };             /* adc ebp, 0 */
    static const uint8_t f4[] = { 0x83, 0x04, 0x24, 0x03, 0xc3 };       /* add dword [esp], 3 */
    const uint32_t code = low + CODE, table = low + TABLE;
    const uint32_t targets[4] = { code + F2, code + F1, code + F3, code + F4 };
    uint8_t image[0xc0];
    Run emitter, plain, predicted;

    memset(image, 0xcc, sizeof(image));
    memcpy(image, program, sizeof(program));
    memcpy(image + 1, &table, 4);
    memcpy(image + F1, f1, sizeof(f1));
    memcpy(image + F2, f2, sizeof(f2));
    memcpy(image + F3, f3, sizeof(f3));
    memcpy(image + F4, f4, sizeof(f4));
    memcpy(guest + TABLE, targets, sizeof(targets));
    emitter = run(image, sizeof(image), 0);
    plain = run_superblocks(image, sizeof(image));
    call_predict = 1;
    predicted = run_superblocks(image, sizeof(image));
    call_predict = 0;
    assert(emitter.status == PW_OK && emitter.state.eip == 0xdead0000u);
    assert(emitter.state.gpr[3] == 0x44444444u + 150 && emitter.state.gpr[7] == low + DATA + 0x100 + 150);
    assert(emitter.state.gpr[5] == 0x66666666u + 49 && emitter.state.gpr[1] == 0 && emitter.state.gpr[6] == 0);
    same(&plain, &emitter);
    same(&predicted, &emitter);
    assert(predicted.reencoded && plain.reencoded);
}

/* A branchy loop whose side exits are taken on alternate iterations, and
 * one taken from the fallthrough of another: with superblocks the result
 * matches the emitter, and once each side exit has linked itself the loop
 * no longer returns to the dispatcher. */
static void test_superblocks(void)
{
    static const uint8_t code[] = {
        0xb9, 0xa0, 0x0f, 0, 0,             /* 00 mov ecx, 4000 */
        0x31, 0xc0,                         /* 05 xor eax, eax */
        0x31, 0xdb,                         /* 07 xor ebx, ebx */
        0xf6, 0xc1, 0x01,                   /* 09 L: test cl, 1 */
        0x74, 0x05,                         /* 0c jz E (side exit) */
        0x83, 0xc0, 0x03,                   /* 0e add eax, 3 */
        0xeb, 0x03,                         /* 11 jmp J */
        0x83, 0xc3, 0x05,                   /* 13 E: add ebx, 5 */
        0xf6, 0xc1, 0x02,                   /* 16 J: test cl, 2 */
        0x75, 0x01,                         /* 19 jnz K (side exit) */
        0x40,                               /* 1b inc eax */
        0x39, 0xd8,                         /* 1c K: cmp eax, ebx */
        0x49,                               /* 1e dec ecx */
        0x75, 0xe8,                         /* 1f jnz L (last: the block's exit) */
        0xc3,                               /* 21 ret */
    };
    Run emitter = run(code, sizeof(code), 0), super = run_superblocks(code, sizeof(code));

    assert(emitter.status == PW_OK && emitter.state.eip == 0xdead0000u);
    same(&super, &emitter);
    if (super.steps > 40) fprintf(stderr, "superblocks: %u steps\n", super.steps);
    assert(super.steps <= 40);
}

/* SSE, SSE2, x87 and MMX with native FP: the re-encoder copies them onto the
 * guest's own state in the host FPU, and the result matches what wowprospero
 * did before (the emitter and its software x87, the rest one instruction at
 * a time on the host): registers, flags, memory, xmm0-7 and the x87 and
 * MXCSR state; the re-encoder took the FP blocks. Only TOP is compared from
 * fnstsw: the software x87 leaves C1 where the CPU clears it. */
/* String instructions as the host's (movs, stos, scas and cmps, with rep,
 * repe and repne, bytes, words and dwords, a zero count) forwards, and
 * backwards after std: the same as the emitter and the host fallback in
 * every mode. The
 * emitter's lods leaves eax as it was, so lods is checked against its
 * expected values instead: dwords, words and bytes, forwards and back. */
static void test_strings(void)
{
    static const uint8_t code[] = {
        0xb9, 0x10, 0x00, 0x00, 0x00, 0xf3, 0xa4, 0xb9, 0x04, 0x00, 0x00, 0x00,
        0xf3, 0xa5, 0x66, 0xa5, 0xa4, 0xb0, 0x41, 0xb9, 0x08, 0x00, 0x00, 0x00,
        0xf3, 0xaa, 0xab, 0x8d, 0x7f, 0xc0, 0xb9, 0x20, 0x00, 0x00, 0x00, 0xf2,
        0xae, 0x89, 0xcb, 0x83, 0xee, 0x40, 0xb9, 0x0a, 0x00, 0x00, 0x00, 0xf3,
        0xa6, 0x0f, 0x95, 0xc2, 0x89, 0xcd, 0xfd, 0x8d, 0x76, 0x20, 0x8d, 0xbf,
        0x80, 0x00, 0x00, 0x00, 0xb9, 0x04, 0x00, 0x00, 0x00, 0xf3, 0xa5, 0xa4,
        0xfc, 0x31, 0xc9, 0xf3, 0xa4, 0xc3,
    };
    static const uint8_t lods[] = {
        0x8d, 0x76, 0x23, 0xad, 0x89, 0xc3, 0x66, 0xad, 0xfd, 0xac, 0xfc, 0xc3,
    };
    Run r;

    /* As in wowprospero, what neither translator takes (std, cld, scas on
     * the emitter's side) runs on the host. */
    hostexec_fallback = 1;
    compare(code, sizeof(code));
    r = run_superblocks(lods, sizeof(lods));
    hostexec_fallback = 0;
    assert(r.status == PW_OK && r.state.eip == 0xdead0000u);
    assert(r.state.gpr[3] == pattern(35));                              /* lodsl */
    assert((r.state.gpr[0] & 0xffffff00u) == ((pattern(35) & 0xffff0000u) | (pattern(39) & 0xff00u)));
    assert((r.state.gpr[0] & 0xff) == (pattern(41) & 0xff));             /* lodsb after std */
    assert(r.state.gpr[6] == low + DATA + 40);                          /* esi went back one */
}

static void test_native_fp(void)
{
    static const uint8_t code[] = {
        0xc7, 0x06, 0x00, 0x00, 0x80, 0x3f, 0xc7, 0x46, 0x04, 0x00, 0x00, 0x00,
        0x40, 0xc7, 0x46, 0x08, 0x00, 0x00, 0x40, 0x40, 0xc7, 0x46, 0x0c, 0x00,
        0x00, 0x80, 0x40, 0xc7, 0x46, 0x10, 0x00, 0x00, 0x00, 0x3f, 0xc7, 0x46,
        0x14, 0x00, 0x00, 0x80, 0x3e, 0xc7, 0x46, 0x18, 0x00, 0x00, 0xc0, 0xbf,
        0xc7, 0x46, 0x1c, 0x00, 0x00, 0x00, 0x41, 0xc7, 0x46, 0x20, 0x00, 0x00,
        0x00, 0x00, 0xc7, 0x46, 0x24, 0x00, 0x00, 0x02, 0x40, 0xc7, 0x46, 0x28,
        0x07, 0x00, 0x00, 0x00, 0x0f, 0x10, 0x06, 0x0f, 0x10, 0x4e, 0x10, 0x0f,
        0x58, 0xc1, 0x0f, 0x59, 0xc8, 0x0f, 0x28, 0xd1, 0x0f, 0xc6, 0xd2, 0x1b,
        0xf2, 0x0f, 0x10, 0x5e, 0x20, 0xf2, 0x0f, 0x51, 0xe3, 0xf2, 0x0f, 0x2a,
        0xe9, 0xf2, 0x0f, 0x58, 0xec, 0xf2, 0x0f, 0x2c, 0xdd, 0x66, 0x0f, 0x2f,
        0xeb, 0x0f, 0x97, 0xc0, 0x66, 0x0f, 0x6e, 0xf2, 0x66, 0x0f, 0x70, 0xf6,
        0x00, 0x66, 0x0f, 0xef, 0xff, 0x66, 0x0f, 0xfe, 0xfe, 0xf3, 0x0f, 0x7f,
        0x76, 0x30, 0x0f, 0x11, 0x46, 0x40, 0x66, 0x0f, 0xd7, 0xd1, 0xdd, 0x46,
        0x20, 0xdb, 0x46, 0x28, 0xde, 0xc9, 0xd9, 0xfa, 0xd9, 0xe8, 0xde, 0xc1,
        0xdd, 0x56, 0x50, 0xdb, 0x5e, 0x58, 0xd9, 0xe8, 0xd9, 0xee, 0xdf, 0xf1,
        0xdd, 0xd8, 0xdf, 0xe0, 0x25, 0x00, 0x38, 0x00, 0x00, 0x0f, 0x6f, 0x06,
        0x0f, 0xfd, 0x46, 0x08, 0x0f, 0x7f, 0x46, 0x60, 0x0f, 0x77, 0xc3,
    };
    Run emitter, native;

    hostexec_fallback = 1;
    emitter = run(code, sizeof(code), 0);
    native_fp = 1;
    native = run(code, sizeof(code), 1);
    native_fp = 0;
    hostexec_fallback = 0;
    if (emitter.status != PW_OK || emitter.state.eip != 0xdead0000u)
        fprintf(stderr, "native fp: emitter status %d eip +%x\n", emitter.status, emitter.state.eip - low - CODE);
    assert(emitter.status == PW_OK && emitter.state.eip == 0xdead0000u);
    same(&native, &emitter);
    for (unsigned r = 0; r < 8; r++) {
        if (memcmp(native.state.fp.xmm[r], emitter.state.fp.xmm[r], 16))
            fprintf(stderr, "native fp: xmm%u differs\n", r);
        assert(!memcmp(native.state.fp.xmm[r], emitter.state.fp.xmm[r], 16));
    }
    assert(native.state.fp.mxcsr == emitter.state.fp.mxcsr);
    assert(native.state.fp.x87_control == emitter.state.fp.x87_control);
    assert((native.state.fp.x87_status & 0x3800) == (emitter.state.fp.x87_status & 0x3800));
    assert(native.reencoded >= 1);
}

/* Every family the re-encoder copies with native FP, in register and memory
 * form: SSE moves, arithmetic, compares and conversions (with GPRs), SSE2
 * integer, SSE3, SSSE3 and SSE4.1, MXCSR, fences and prefetch, MMX, and x87
 * at every memory width with fcmov, fcomi, fucomi and fstsw, sahf and lahf.
 * Same comparison as
 * test_native_fp; the SSE4.1 forms with a GPR, which the host fallback does
 * not take, are in test_native_fp_gpr. */
/* The SSE4.1 forms with a GPR or memory operand (pextrd, pextrb, pinsrd,
 * pinsrb, extractps), edi being r13 on the host: their results against the
 * guest's data. */
static void test_native_fp_gpr(void)
{
    static const uint8_t code[] = {
        0xf3, 0x0f, 0x6f, 0x1e, 0x66, 0x0f, 0xef, 0xf6, 0x66, 0x0f, 0x3a, 0x16,
        0xdf, 0x01, 0x66, 0x0f, 0x3a, 0x14, 0x5e, 0x40, 0x02, 0xba, 0x44, 0x33,
        0x22, 0x11, 0x66, 0x0f, 0x3a, 0x22, 0xf2, 0x02, 0x66, 0x0f, 0x3a, 0x20,
        0x76, 0x08, 0x01, 0x66, 0x0f, 0x3a, 0x17, 0xd9, 0x03, 0xf3, 0x0f, 0x7f,
        0x76, 0x50, 0xc3,
    };
    Run native;
    uint32_t dword[4];
    uint8_t lane[16] = { 0 };

    native_fp = 1;
    native = run(code, sizeof(code), 1);
    native_fp = 0;
    assert(native.status == PW_OK && native.state.eip == 0xdead0000u);
    for (unsigned k = 0; k < 4; k++) dword[k] = pattern(4 * k);
    assert(native.state.gpr[7] == dword[1]);                  /* pextrd into r13 */
    assert(native.data[64] == (uint8_t)(dword[0] >> 16));     /* pextrb to memory */
    assert(native.state.gpr[1] == dword[3]);                  /* extractps */
    lane[1] = (uint8_t)pattern(8);                            /* pinsrb from memory */
    memcpy(lane + 8, &(uint32_t){ 0x11223344u }, 4);          /* pinsrd */
    assert(!memcmp(native.data + 80, lane, 16));
    assert(native.reencoded >= 1);
}

/* Re-encoded FP code around an instruction only the emitter takes (div):
 * the chain leaves through C at each crossing, which never links the two
 * kinds, and the guest's xmm and x87 values live across it survive the
 * emitter, which uses xmm as scratch. Also pause, and cpuid, which the
 * host-exec stepper runs with the guest's feature mask (as wowprospero
 * does). */
static void test_native_fp_emitter(void)
{
    static const uint8_t code[] = {
        0xc7, 0x46, 0x20, 0x00, 0x00, 0x00, 0x00, 0xc7, 0x46, 0x24, 0x00, 0x00,
        0x02, 0x40, 0xb9, 0x05, 0x00, 0x00, 0x00, 0xf2, 0x0f, 0x10, 0x56, 0x20,
        0x66, 0x0f, 0x57, 0xc9, 0xdd, 0x46, 0x20, 0xf2, 0x0f, 0x58, 0xca, 0xf3,
        0x90, 0xb8, 0x64, 0x00, 0x00, 0x00, 0x31, 0xd2, 0xbb, 0x07, 0x00, 0x00,
        0x00, 0xf7, 0xf3, 0xd8, 0xc0, 0x49, 0x75, 0xe7, 0xf2, 0x0f, 0x11, 0x4e,
        0x40, 0xdd, 0x5e, 0x48, 0x89, 0x46, 0x50, 0x31, 0xc0, 0x0f, 0xa2, 0xc3,
    };
    Run native;
    double sum, doubled;
    uint32_t a, b, c, d;

    native_fp = 1;
    hostexec_fallback = 1;
    native = run(code, sizeof(code), 1);
    hostexec_fallback = 0;
    native_fp = 0;
    assert(native.status == PW_OK && native.state.eip == 0xdead0000u);
    memcpy(&sum, native.data + 64, 8);
    memcpy(&doubled, native.data + 72, 8);
    assert(sum == 5 * 2.25 && doubled == 2.25 * 32);
    assert(native.data[80] == 100 / 7);
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
    assert(native.state.gpr[3] == b && native.state.gpr[1] == c && native.state.gpr[2] == d);
    assert(native.reencoded >= 2);
}

static void test_native_fp_forms(void)
{
    static const uint8_t code[] = {
        0xc7, 0x06, 0x00, 0x00, 0x80, 0x3f, 0xc7, 0x46, 0x04, 0x00, 0x00, 0x00,
        0x40, 0xc7, 0x46, 0x08, 0x00, 0x00, 0x40, 0x40, 0xc7, 0x46, 0x0c, 0x00,
        0x00, 0x80, 0x40, 0xc7, 0x46, 0x10, 0x00, 0x00, 0x00, 0x3f, 0xc7, 0x46,
        0x14, 0x00, 0x00, 0x80, 0x3e, 0xc7, 0x46, 0x18, 0x00, 0x00, 0xc0, 0xbf,
        0xc7, 0x46, 0x1c, 0x00, 0x00, 0x00, 0x41, 0xc7, 0x46, 0x20, 0x00, 0x00,
        0x00, 0x00, 0xc7, 0x46, 0x24, 0x00, 0x00, 0x02, 0x40, 0xc7, 0x46, 0x28,
        0x07, 0x00, 0x00, 0x00, 0xc7, 0x46, 0x2c, 0x00, 0x00, 0xf0, 0x3f, 0xc7,
        0x46, 0x30, 0x00, 0x00, 0x00, 0x00, 0xc7, 0x46, 0x34, 0x00, 0x00, 0x10,
        0x40, 0xc7, 0x46, 0x38, 0x02, 0x00, 0x03, 0x00, 0xc7, 0x46, 0x3c, 0x04,
        0x00, 0x05, 0x00, 0x8d, 0xae, 0x80, 0x00, 0x00, 0x00, 0x0f, 0x10, 0x06,
        0x0f, 0x10, 0x4e, 0x10, 0xf3, 0x0f, 0x10, 0x56, 0x04, 0xf2, 0x0f, 0x10,
        0x5e, 0x20, 0x66, 0x0f, 0x10, 0x26, 0x0f, 0x12, 0x6e, 0x08, 0x0f, 0x16,
        0x6e, 0x10, 0x66, 0x0f, 0x12, 0x76, 0x20, 0x66, 0x0f, 0x16, 0x76, 0x30,
        0x0f, 0x12, 0xf9, 0x0f, 0x16, 0xf8, 0x0f, 0x14, 0xc1, 0x0f, 0x15, 0x56,
        0x10, 0x0f, 0x28, 0xc8, 0x66, 0x0f, 0x28, 0x56, 0x10, 0x0f, 0x11, 0x45,
        0x00, 0xf3, 0x0f, 0x11, 0x55, 0x10, 0xf2, 0x0f, 0x11, 0x5d, 0x18, 0x0f,
        0x13, 0x6d, 0x20, 0x0f, 0x17, 0x6d, 0x28, 0x0f, 0x29, 0x4d, 0x30, 0x0f,
        0x2b, 0x4d, 0x40, 0x0f, 0x58, 0xc1, 0xf3, 0x0f, 0x58, 0x46, 0x04, 0xf2,
        0x0f, 0x58, 0x5e, 0x20, 0x0f, 0x5c, 0xc8, 0x0f, 0x59, 0x0e, 0xf2, 0x0f,
        0x59, 0xdb, 0x0f, 0x5e, 0xca, 0xf2, 0x0f, 0x5e, 0x5e, 0x20, 0x0f, 0x51,
        0xe1, 0xf3, 0x0f, 0x51, 0xe2, 0x0f, 0x5d, 0xc8, 0xf2, 0x0f, 0x5f, 0x5e,
        0x20, 0x0f, 0x54, 0xc1, 0x0f, 0x55, 0xca, 0x0f, 0x56, 0xd0, 0x0f, 0x57,
        0xff, 0x0f, 0xc2, 0xc1, 0x01, 0xf2, 0x0f, 0xc2, 0x5e, 0x20, 0x02, 0x0f,
        0xc6, 0xc1, 0x4e, 0x66, 0x0f, 0xc6, 0xdb, 0x01, 0x0f, 0x2f, 0xd1, 0x0f,
        0x97, 0xc3, 0x66, 0x0f, 0x2e, 0x5e, 0x20, 0x0f, 0x92, 0xc7, 0x0f, 0x5a,
        0xe1, 0x66, 0x0f, 0x5a, 0xec, 0xf3, 0x0f, 0x5a, 0x76, 0x04, 0xf2, 0x0f,
        0x5a, 0xf3, 0x0f, 0x5b, 0x3e, 0xf3, 0x0f, 0x5b, 0xf9, 0xf3, 0x0f, 0x2a,
        0xc1, 0xf2, 0x0f, 0x2a, 0x4e, 0x28, 0xf3, 0x0f, 0x2c, 0xd0, 0xf2, 0x0f,
        0x2d, 0x7e, 0x20, 0xf2, 0x0f, 0x2c, 0xcb, 0x0f, 0x50, 0xc1, 0x66, 0x0f,
        0x50, 0xd3, 0x66, 0x0f, 0x6e, 0xc1, 0x66, 0x0f, 0x6e, 0x4e, 0x28, 0x66,
        0x0f, 0x7e, 0xc0, 0x66, 0x0f, 0x7e, 0x4d, 0x50, 0xf3, 0x0f, 0x7e, 0x56,
        0x38, 0x66, 0x0f, 0xd6, 0x55, 0x58, 0xf3, 0x0f, 0x6f, 0x1e, 0x66, 0x0f,
        0x6f, 0x66, 0x10, 0xf3, 0x0f, 0x7f, 0x5d, 0x60, 0x66, 0x0f, 0x7f, 0x65,
        0x70, 0x66, 0x0f, 0xfe, 0xe3, 0x66, 0x0f, 0xf9, 0x66, 0x10, 0x66, 0x0f,
        0xd5, 0xe3, 0x66, 0x0f, 0xf4, 0xe3, 0x66, 0x0f, 0xdb, 0xe3, 0x66, 0x0f,
        0xeb, 0x26, 0x66, 0x0f, 0xef, 0xd2, 0x66, 0x0f, 0x76, 0xd3, 0x66, 0x0f,
        0x64, 0xd4, 0x66, 0x0f, 0x60, 0xd3, 0x66, 0x0f, 0x6a, 0x56, 0x10, 0x66,
        0x0f, 0x6c, 0xd3, 0x66, 0x0f, 0x63, 0xd3, 0x66, 0x0f, 0x67, 0xd4, 0x66,
        0x0f, 0x70, 0xeb, 0x1b, 0xf2, 0x0f, 0x70, 0x2e, 0x1b, 0xf3, 0x0f, 0x70,
        0xeb, 0x1b, 0x66, 0x0f, 0x71, 0xf5, 0x03, 0x66, 0x0f, 0x72, 0xe5, 0x02,
        0x66, 0x0f, 0x73, 0xdd, 0x04, 0x66, 0x0f, 0xf3, 0xea, 0x66, 0x0f, 0xf5,
        0xeb, 0x66, 0x0f, 0xe0, 0xeb, 0x66, 0x0f, 0xf6, 0xeb, 0x66, 0x0f, 0xda,
        0xeb, 0x66, 0x0f, 0xd7, 0xdd, 0x66, 0x0f, 0xc4, 0xe9, 0x03, 0x66, 0x0f,
        0xc4, 0x6e, 0x38, 0x01, 0x66, 0x0f, 0xc5, 0xd5, 0x03, 0x0f, 0xc3, 0x4d,
        0x78, 0x66, 0x0f, 0xe7, 0xad, 0x80, 0x00, 0x00, 0x00, 0xf2, 0x0f, 0x12,
        0x76, 0x20, 0xf3, 0x0f, 0x12, 0xf1, 0xf3, 0x0f, 0x16, 0x36, 0xf2, 0x0f,
        0x7c, 0xf1, 0x66, 0x0f, 0x7d, 0x76, 0x20, 0xf2, 0x0f, 0xd0, 0xf1, 0xf2,
        0x0f, 0xf0, 0x3e, 0x66, 0x0f, 0x38, 0x00, 0xfb, 0x66, 0x0f, 0x3a, 0x0f,
        0xfb, 0x04, 0x66, 0x0f, 0x38, 0x1e, 0x3e, 0x66, 0x0f, 0x38, 0x04, 0xfb,
        0x66, 0x0f, 0x38, 0x39, 0xfb, 0x66, 0x0f, 0x38, 0x40, 0x7e, 0x10, 0x66,
        0x0f, 0x38, 0x17, 0xfb, 0x0f, 0x94, 0xc0, 0x66, 0x0f, 0x3a, 0x0b, 0xf3,
        0x01, 0x66, 0x0f, 0x3a, 0x0c, 0xf1, 0x05, 0x0f, 0xae, 0x9d, 0x8c, 0x00,
        0x00, 0x00, 0x0f, 0xae, 0x95, 0x8c, 0x00, 0x00, 0x00, 0x0f, 0xae, 0xe8,
        0x0f, 0xae, 0xf0, 0x0f, 0xae, 0xf8, 0x0f, 0x18, 0x0e, 0x0f, 0x18, 0x46,
        0x40, 0x0f, 0x6f, 0x06, 0x0f, 0x6f, 0x4e, 0x08, 0x0f, 0xfd, 0xc1, 0x0f,
        0xfa, 0x46, 0x10, 0x0f, 0xd5, 0xc1, 0x0f, 0x73, 0xf0, 0x03, 0x0f, 0x60,
        0xc1, 0x0f, 0x70, 0xd0, 0x1b, 0x0f, 0x7e, 0xd2, 0x0f, 0x6e, 0xd9, 0x0f,
        0x7f, 0x85, 0x90, 0x00, 0x00, 0x00, 0xf3, 0x0f, 0xd6, 0xc8, 0xf2, 0x0f,
        0xd6, 0xe2, 0x0f, 0x2a, 0xd1, 0x0f, 0x2c, 0xea, 0x66, 0x0f, 0x2a, 0x5e,
        0x08, 0x0f, 0x77, 0xd9, 0x46, 0x04, 0xdd, 0x46, 0x20, 0xde, 0xc1, 0xd8,
        0x06, 0xdc, 0x4e, 0x20, 0xd8, 0x6e, 0x08, 0xdc, 0x76, 0x20, 0xda, 0x46,
        0x28, 0xde, 0x4e, 0x38, 0xdb, 0x46, 0x28, 0xdf, 0x6e, 0x30, 0xdf, 0x46,
        0x38, 0xd9, 0xca, 0xda, 0xc1, 0xdb, 0xca, 0xda, 0xd1, 0xda, 0xda, 0xdb,
        0xe9, 0x0f, 0x92, 0xc0, 0xdb, 0xf2, 0x0f, 0x97, 0xc4, 0xd9, 0x9d, 0x98,
        0x00, 0x00, 0x00, 0xdd, 0x9d, 0xa0, 0x00, 0x00, 0x00, 0xdb, 0x9d, 0xa8,
        0x00, 0x00, 0x00, 0xdb, 0x8d, 0xac, 0x00, 0x00, 0x00, 0xdb, 0xad, 0x98,
        0x00, 0x00, 0x00, 0xdb, 0xbd, 0xb0, 0x00, 0x00, 0x00, 0xd9, 0xbd, 0xbc,
        0x00, 0x00, 0x00, 0xd9, 0xad, 0xbc, 0x00, 0x00, 0x00, 0xd9, 0xb5, 0xc0,
        0x00, 0x00, 0x00, 0xd9, 0xa5, 0xc0, 0x00, 0x00, 0x00, 0xdd, 0xbd, 0xe0,
        0x00, 0x00, 0x00, 0xd9, 0xeb, 0xd9, 0xea, 0xd9, 0xe0, 0xd9, 0xe1, 0xd9,
        0xfa, 0xd9, 0xf8, 0xd9, 0xfc, 0xd9, 0xfd, 0xd9, 0xf1, 0xdd, 0xd8, 0xd9,
        0xe8, 0xdf, 0x9d, 0xe8, 0x00, 0x00, 0x00, 0xd9, 0xe8, 0xdf, 0xbd, 0xf0,
        0x00, 0x00, 0x00, 0xd9, 0xe8, 0xd9, 0xee, 0xde, 0xd9, 0x9b, 0xdf, 0xe0,
        0x9e, 0x0f, 0x92, 0xc1, 0x0f, 0x94, 0xc5, 0x9f, 0x25, 0x00, 0xd5, 0x00,
        0x00, 0xdb, 0xe3, 0xc7, 0x85, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0xc7, 0x85, 0xc4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc7,
        0x85, 0xc8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc7, 0x85, 0xcc,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc7, 0x85, 0xd0, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0xc7, 0x85, 0xd4, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0xc7, 0x85, 0xd8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0xc7, 0x85, 0xdc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc7,
        0x85, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc3,
    };
    Run reference, native;

    hostexec_fallback = 1;
    reference = run(code, sizeof(code), 0);
    native_fp = 1;
    native = run(code, sizeof(code), 1);
    native_fp = 0;
    hostexec_fallback = 0;
    if (reference.status != PW_OK || reference.state.eip != 0xdead0000u)
        fprintf(stderr, "native fp forms: reference status %d eip +%x\n", reference.status,
                reference.state.eip - low - CODE);
    if (native.status != PW_OK || native.state.eip != 0xdead0000u)
        fprintf(stderr, "native fp forms: native status %d eip +%x\n", native.status,
                native.state.eip - low - CODE);
    for (unsigned k = 0; k < sizeof(native.data); k++)
        if (native.data[k] != reference.data[k]) {
            fprintf(stderr, "native fp forms: data +%#x %02x != %02x\n", k, native.data[k], reference.data[k]);
            break;
        }
    same(&native, &reference);
    for (unsigned r = 0; r < 8; r++) {
        if (memcmp(native.state.fp.xmm[r], reference.state.fp.xmm[r], 16))
            fprintf(stderr, "native fp forms: xmm%u differs\n", r);
        assert(!memcmp(native.state.fp.xmm[r], reference.state.fp.xmm[r], 16));
    }
    assert(native.state.fp.mxcsr == reference.state.fp.mxcsr);
    assert(native.reencoded >= 1);
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

static void test_prefixed_padding(void)
{
    static const uint8_t padding[] = {
        0x66,0x66,0x66,0x66,0x66,0x66,0x2e,0x0f,0x1f,0x84,0,0,0,0,0,
        0x67,0x0f,0x1f,0x06,0,0,
        0x64,0x65,0x2e,0x66,0x0f,0x1f,0x00,
        0x66,0x66,0x90,0xc3
    };
    Run reference = run(padding,sizeof(padding),0);
    Run translated = run(padding,sizeof(padding),1);
    same(&reference,&translated);
    assert(translated.reencoded && translated.status == PW_OK);
    translated = run_superblocks(padding,sizeof(padding));
    same(&reference,&translated);
    /* Invalid sub-opcodes and overlong padding cannot be normalized to NOP. */
    {
        const uint8_t invalid[]={0x66,0x0f,0x1f,0xc8,0xc3};
        uint8_t overlong[17];
        memset(overlong,0x66,8);memcpy(overlong+8,padding+7,8);overlong[16]=0xc3;
        translated=run(invalid,sizeof(invalid),1);
        assert(translated.status==PW_ERR_UNSUPPORTED && translated.state.eip==low+CODE);
        translated=run(overlong,sizeof(overlong),1);
        assert(translated.status==PW_ERR_UNSUPPORTED && translated.state.eip==low+CODE);
    }
}

int main(void)
{
    guest = mmap(NULL, SPAN, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    assert(guest != MAP_FAILED);
    low = (uint32_t)(uintptr_t)guest;
    {
        static PwVmBackend vm;
        assert(pw_vm_posix_backend(&vm) == PW_OK);
        assert(pw_x86_hostexec_init(&hostexec, &vm, 1u << 16) == PW_OK);
    }
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
    test_prefixed_padding();
    test_registers();
    test_xchg();
    test_atomic_and_segments();
    test_memory();
    test_control();
    test_bnd_branches();
    test_mixed_and_indirect();
    test_two_way_calls();
    test_null_targets();
    test_return_targets();
    test_unbounded_chains();
    test_call_stack();
    test_superblocks();
    test_call_predict_site();
    test_call_predict_engine();
    test_strings();
    test_native_fp();
    test_native_fp_forms();
    test_native_fp_gpr();
    test_native_fp_emitter();
    test_fault();
    printf("reencode passed: options, register remapping, xchg, atomics and segments, memory operands, flags across links, "
           "stack and calls, emitter hand-over, indirect targets, pinned returns, unbounded chains, call stack, superblocks, predicted calls, strings, native FP, fault state\n");
    return 0;
}
