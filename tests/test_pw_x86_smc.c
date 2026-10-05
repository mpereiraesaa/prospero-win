/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Code that guest stores change, with no notification, on writable pages
 * (PwX86SourceViewWritable): blocks translated from there check their
 * source on every entry, as a processor sees code it writes. */
#define _GNU_SOURCE
#include "../src/pw_x86_engine.h"
#include "../src/pw_x86_reencode.h"
#include "../src/pw_vm_posix.h"
#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

/* The guest: code at CODE, one 4 KiB page of it writable (WRITABLE, where
 * the patched functions live); the rest of the code (HOOK) read-only. */
enum { SPAN = 0x40000, CODE = 0x1000, WRITABLE = 0x2000, HOOK = 0x3000, DATA = 0x20000,
       STACK_TOP = 0x3f000, SENTINEL = 0xdead0000u };

static uint8_t *guest;
static uint32_t low;

static int view(void *opaque, uint32_t pc, const uint8_t **data, size_t *bytes)
{
    (void)opaque;
    if (pc < low + CODE || pc >= low + DATA) return PW_ERR_NOT_FOUND;
    *data = (const uint8_t *)(uintptr_t)pc;
    *bytes = low + DATA - pc;
    return PW_OK;
}

/* [low + WRITABLE, low + WRITABLE + 0x1000) is writable; trusting: none is. */
static unsigned trusting;
static int view_writable(void *opaque, uint32_t pc, const uint8_t **data, size_t *bytes, size_t *writable_from)
{
    const uint32_t first = low + WRITABLE, end = first + 0x1000;
    int status = view(opaque, pc, data, bytes);

    *writable_from = SIZE_MAX;
    if (trusting || status != PW_OK) return status;
    if (pc >= first && pc < end) *writable_from = 0;
    else if (pc < first && first - pc < *bytes) *writable_from = first - pc;
    return status;
}

/* The engine configurations: the emitter alone, the re-encoder, and the
 * re-encoder as wowprospero runs it (superblocks, a call stack, native FP,
 * fault markers, predicted calls). */
enum { EMITTER, REENCODE, PRODUCTION, CONFIGS };
static const char *const names[CONFIGS] = { "emitter", "reencode", "production" };

enum { CALL_STACK_BYTES = 0x4000 };
static uint8_t *call_stack_region;
static PwX86Engine *current;
static PwVmBackend posix;

static void on_fault(int sig, siginfo_t *info, void *context)
{
    ucontext_t *uc = context;
    uintptr_t rsp, target;

    (void)sig;
    if (current && pw_x86_engine_call_stack_fault(current, (uintptr_t)info->si_addr, &rsp)) {
        uc->uc_mcontext.gregs[REG_RSP] = (greg_t)rsp;
        return;
    }
    if (current && (target = pw_x86_engine_fault_redirect(current, (uintptr_t)uc->uc_mcontext.gregs[REG_RIP]))) {
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)target;
        return;
    }
    static const char message[] = "unexpected fault\n";
    (void)!write(2, message, sizeof(message) - 1);
    _exit(3);
}

/* Superblocks and predicted calls rewrite their own code. */
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

static PwX86CacheEntry entries[1024];

static void setup(PwX86Engine *engine, unsigned config)
{
    static PwVmBackend vm;

    assert(pw_vm_posix_backend(&posix) == PW_OK);
    vm = posix;
    if (config == PRODUCTION) {
        vm.commit = writable_commit;
        vm.protect = writable_protect;
    }
    assert(pw_x86_engine_init(engine, &vm, entries, 1024, 1u << 20, 1, view, NULL) == PW_OK);
    assert(pw_x86_engine_set_source_view_writable(engine, view_writable) == PW_OK);
    assert(pw_x86_engine_set_chaining(engine, 1) == PW_OK);
    assert(pw_x86_engine_set_indirect(engine, 1) == PW_OK);
    assert(pw_x86_engine_set_counters(engine, 0) == PW_OK);
    assert(pw_x86_engine_set_flat_memory(engine, low, low + SPAN) == PW_OK);
    assert(pw_x86_engine_set_reencode(engine, config != EMITTER) == PW_OK);
    if (config != PRODUCTION) return;
    assert(pw_x86_engine_set_unbounded_chains(engine, 1) == PW_OK);
    assert(pw_x86_engine_set_superblocks(engine, 1) == PW_OK);
    assert(pw_x86_engine_set_call_predict(engine, 1) == PW_OK);
    assert(pw_x86_engine_set_native_fp(engine, 1) == PW_OK);
    assert(pw_x86_engine_set_fault_markers(engine, 1) == PW_OK);
    assert(pw_x86_engine_set_call_stack(engine, call_stack_region + PW_X86_ENGINE_CALL_STACK_GUARD,
                                        CALL_STACK_BYTES) == PW_OK);
}

typedef struct Run {
    PwX86State state;
    int status;
    unsigned steps;
    uint64_t compiles, stale, retired, revived;
    unsigned verified, verified_writable, verified_elsewhere;
} Run;

/* Run the guest from low + CODE until it returns to SENTINEL, with no memory
 * notification of any kind and no engine reset in between. */
static Run run(unsigned config)
{
    PwX86Engine engine;
    PwX86StepReport report;
    Run r;

    setup(&engine, config);
    memset(&r, 0, sizeof(r));
    r.state.eip = low + CODE;
    r.state.stack_low = low;
    r.state.stack_high = low + SPAN;
    r.state.memory_count = 1;
    r.state.memory[0].low = low;
    r.state.memory[0].high = (uint64_t)low + SPAN;
    r.state.memory[0].permissions = PW_X86_READ | PW_X86_WRITE;
    r.state.gpr[4] = low + STACK_TOP;
    r.state.eflags = 0x2;
    pw_guest_fp_init(&r.state.fp);
    memcpy(guest + STACK_TOP, &(uint32_t){ SENTINEL }, 4);
    current = &engine;
    r.status = PW_OK;
    while (r.steps < 200000 && r.state.eip != SENTINEL && r.status == PW_OK) {
        r.status = pw_x86_engine_step(&engine, &r.state, &report);
        r.steps++;
    }
    current = NULL;
    pw_x86_engine_fp_sync(&engine, &r.state);
    r.compiles = engine.compiles;
    r.stale = engine.stale_blocks;
    r.retired = engine.retired_total;
    r.revived = engine.revived_blocks;
    for (unsigned k = 0; k < engine.cache.capacity; k++) {
        const PwX86CacheEntry *e = &engine.cache.entries[k];
        if (!e->used || !e->verify) continue;
        r.verified++;
        if (e->guest_pc >= low + WRITABLE && e->guest_pc < low + WRITABLE + 0x1000) r.verified_writable++;
        else if (e->guest_pc + e->source_bytes <= low + WRITABLE) r.verified_elsewhere++;
        /* A checked block keeps the bytes it was translated from. */
        assert(e->source_copy_offset && e->source_copy_offset + e->source_bytes <= e->code_bytes);
        assert(e->verify == (config == EMITTER || !pw_x86_reencoded(&e->entry_contract)
                             ? PW_X86_VERIFY_DISPATCH : PW_X86_VERIFY_ENTRY));
    }
    assert(pw_x86_engine_destroy(&engine) == PW_OK);
    return r;
}

static void place(uint32_t offset, const uint8_t *code, size_t bytes)
{
    memcpy(guest + offset, code, bytes);
}

static void put32(uint8_t *at, uint32_t value)
{
    memcpy(at, &value, 4);
}

static void reset_code(void)
{
    memset(guest + CODE, 0xcc, DATA - CODE);
    memset(guest + DATA, 0, 0x1000);
}

/* f() returns 1 from writable code; the guest rewrites it with a plain store
 * to return 2 and calls it again, then rewrites it to read the carry flag
 * (sbb eax, eax), which its first form never read, and calls it with the
 * carry set. Translations that trust read-only code see none of it. */
static void test_rewritten_function(void)
{
    const uint32_t f = low + WRITABLE;
    uint8_t main[64];
    size_t n = 0;

    for (unsigned config = 0; config < CONFIGS; config++) {
        for (trusting = 0; trusting < 2; trusting++) {
            static const uint8_t f1[] = { 0xb8, 1, 0, 0, 0, 0xc3 };    /* mov eax, 1; ret */
            reset_code();
            place(WRITABLE, f1, sizeof(f1));
            n = 0;
            main[n++] = 0xe8; put32(main + n, f - (low + CODE + n + 4)); n += 4;   /* call f */
            main[n++] = 0x89; main[n++] = 0xc3;                                    /* mov ebx, eax */
            main[n++] = 0xc6; main[n++] = 0x05; put32(main + n, f + 1); n += 4;
            main[n++] = 2;                                                         /* mov byte [f+1], 2 */
            main[n++] = 0xe8; put32(main + n, f - (low + CODE + n + 4)); n += 4;   /* call f */
            main[n++] = 0x89; main[n++] = 0xc6;                                    /* mov esi, eax */
            /* f: xor eax, eax (no flag read); then sbb eax, eax. */
            main[n++] = 0xc7; main[n++] = 0x05; put32(main + n, f); n += 4;
            put32(main + n, 0xc3c03190u); n += 4;                                  /* nop; xor eax, eax; ret */
            main[n++] = 0xe8; put32(main + n, f - (low + CODE + n + 4)); n += 4;   /* call f */
            main[n++] = 0x89; main[n++] = 0xc5;                                    /* mov ebp, eax */
            main[n++] = 0xc7; main[n++] = 0x05; put32(main + n, f); n += 4;
            put32(main + n, 0xc3c01990u); n += 4;                                  /* nop; sbb eax, eax; ret */
            main[n++] = 0x31; main[n++] = 0xc9;                                    /* xor ecx, ecx */
            main[n++] = 0x83; main[n++] = 0xe9; main[n++] = 1;                     /* sub ecx, 1: CF */
            main[n++] = 0xe8; put32(main + n, f - (low + CODE + n + 4)); n += 4;   /* call f */
            main[n++] = 0xc3;                                                      /* ret */
            place(CODE, main, n);
            Run r = run(config);
            if (r.status != PW_OK || r.state.eip != SENTINEL)
                fprintf(stderr, "%s: status %d eip +%x trusting %u\n", names[config], r.status, r.state.eip - low, trusting);
            assert(r.status == PW_OK && r.state.eip == SENTINEL);
            assert(r.state.gpr[3] == 1);
            if (trusting) {
                /* Read-only code: no block checks itself, so the first f
                 * runs every time, as the engine was promised. */
                assert(!r.verified && !r.stale);
                assert(r.state.gpr[6] == 1 && r.state.gpr[5] == 1 && r.state.gpr[0] == 1);
                continue;
            }
            assert(r.state.gpr[6] == 2 && r.state.gpr[5] == 0 && r.state.gpr[0] == 0xffffffffu);
            /* f's three rewrites each retired the block before; main, in
             * the read-only page, never checks itself. */
            assert(r.retired == 3 && r.verified_writable == 4 && !r.verified_elsewhere);
            assert(r.verified == 4);
        }
    }
    trusting = 0;
}

/* Mod Loader's hook of CText::Get (fxt.hpp's GxtHook): g() starts with a
 * jmp to the hook, in read-only code. For the keys it does not own, the
 * hook puts g's own first five bytes back with plain stores, calls g, and
 * puts its jmp back, every call; no protection change and no instruction
 * cache flush. The hook must run at depth one, g's body once per call; a
 * translation that missed the restore would re-enter the hook until the
 * stack ran out. The hook calls g directly (indirect = 0) or through a
 * pointer in memory, as a function pointer to the original does. */
static Run modloader(unsigned config, unsigned indirect, unsigned calls)
{
    const uint32_t g = low + WRITABLE + 0x100, hook = low + HOOK;
    const uint32_t depth = low + DATA, deepest = low + DATA + 4, count = low + DATA + 8, pointer = low + DATA + 12;
    static const uint8_t original[] = { 0xb8, 7, 0, 0, 0,             /* mov eax, 7 */
                                        0xff, 0x05, 0, 0, 0, 0,       /* inc dword [count] */
                                        0xc3 };                       /* ret */
    uint8_t jump[5], body[sizeof(original)], code[128];
    uint32_t first, fifth;
    size_t n = 0;

    reset_code();
    memcpy(body, original, sizeof(body));
    put32(body + 7, count);
    place(WRITABLE + 0x100, body, sizeof(body));
    jump[0] = 0xe9; put32(jump + 1, hook - (g + 5));
    place(WRITABLE + 0x100, jump, sizeof(jump));                       /* the hook goes in */
    memcpy(&first, body, 4);
    memcpy(&fifth, jump, 4);
    put32(guest + DATA + 12, g);

    /* main: ecx = calls; loop: push ecx; call g; add ebx, eax; pop ecx; dec ecx; jnz loop; ret */
    code[n++] = 0xb9; put32(code + n, calls); n += 4;
    code[n++] = 0x31; code[n++] = 0xdb;                                    /* xor ebx, ebx */
    const size_t loop = n;
    code[n++] = 0x51;                                                      /* push ecx */
    code[n++] = 0xe8; put32(code + n, g - (low + CODE + n + 4)); n += 4;   /* call g */
    code[n++] = 0x01; code[n++] = 0xc3;                                    /* add ebx, eax */
    code[n++] = 0x59;                                                      /* pop ecx */
    code[n++] = 0x49;                                                      /* dec ecx */
    code[n++] = 0x75; code[n] = (uint8_t)(loop - (n + 1)); n++;            /* jnz loop */
    /* Then a third form, mov eax, 9, which neither translation holds: the
     * two redirect to each other until their hops run out. */
    code[n++] = 0xc7; code[n++] = 0x05; put32(code + n, g); n += 4;
    put32(code + n, 0x000009b8u); n += 4;                                  /* mov dword [g], mov eax, 9 */
    code[n++] = 0xc6; code[n++] = 0x05; put32(code + n, g + 4); n += 4;
    code[n++] = 0;                                                         /* mov byte [g+4], 0 */
    code[n++] = 0xe8; put32(code + n, g - (low + CODE + n + 4)); n += 4;   /* call g */
    code[n++] = 0x01; code[n++] = 0xc3;                                    /* add ebx, eax */
    code[n++] = 0xc3;
    place(CODE, code, n);

    /* hook: depth++; deepest = max(deepest, depth); UnHook; call g;
     * MakeHook; depth--; eax += 100; ret */
    n = 0;
    code[n++] = 0xff; code[n++] = 0x05; put32(code + n, depth); n += 4;    /* inc dword [depth] */
    code[n++] = 0xa1; put32(code + n, depth); n += 4;                      /* mov eax, [depth] */
    code[n++] = 0x3b; code[n++] = 0x05; put32(code + n, deepest); n += 4;  /* cmp eax, [deepest] */
    code[n++] = 0x76; code[n++] = 5;                                       /* jbe +5 */
    code[n++] = 0xa3; put32(code + n, deepest); n += 4;                    /* mov [deepest], eax */
    code[n++] = 0xc7; code[n++] = 0x05; put32(code + n, g); n += 4;
    put32(code + n, first); n += 4;                                        /* mov dword [g], original */
    code[n++] = 0xc6; code[n++] = 0x05; put32(code + n, g + 4); n += 4;
    code[n++] = body[4];                                                   /* mov byte [g+4], original */
    if (indirect) { code[n++] = 0xff; code[n++] = 0x15; put32(code + n, pointer); n += 4; } /* call [pointer] */
    else { code[n++] = 0xe8; put32(code + n, g - (hook + n + 4)); n += 4; }                /* call g */
    code[n++] = 0xc7; code[n++] = 0x05; put32(code + n, g); n += 4;
    put32(code + n, fifth); n += 4;                                        /* mov dword [g], jmp */
    code[n++] = 0xc6; code[n++] = 0x05; put32(code + n, g + 4); n += 4;
    code[n++] = jump[4];                                                   /* mov byte [g+4], jmp */
    code[n++] = 0xff; code[n++] = 0x0d; put32(code + n, depth); n += 4;    /* dec dword [depth] */
    code[n++] = 0x83; code[n++] = 0xc0; code[n++] = 100;                   /* add eax, 100 */
    code[n++] = 0xc3;
    place(HOOK, code, n);

    Run r = run(config);
    if (r.status != PW_OK || r.state.eip != SENTINEL)
        fprintf(stderr, "%s: status %d eip %08x steps %u\n", names[config], r.status, r.state.eip, r.steps);
    assert(r.status == PW_OK && r.state.eip == SENTINEL);
    uint32_t deepest_seen, count_seen, depth_now;
    memcpy(&depth_now, guest + DATA, 4);
    memcpy(&deepest_seen, guest + DATA + 4, 4);
    memcpy(&count_seen, guest + DATA + 8, 4);
    assert(deepest_seen == 1 && depth_now == 0);
    assert(count_seen == calls + 1);
    assert(r.state.gpr[3] == calls * 107 + 9);
    assert(guest[WRITABLE + 0x100] == 0xb8 && guest[WRITABLE + 0x101] == 9);
    assert(!r.verified_elsewhere);
    return r;
}

static void test_modloader_hook(void)
{
    for (unsigned config = 0; config < CONFIGS; config++)
        for (unsigned indirect = 0; indirect < 2; indirect++) {
            Run few = modloader(config, indirect, 3), many = modloader(config, indirect, 40);
            /* Each of g's two forms is translated once: later toggles find
             * the earlier translation again instead of translating anew. */
            assert(few.compiles == many.compiles);
            assert(many.revived >= 1);
            /* A re-encoded block that finds its source changed goes on to
             * the translation that matches: after the first toggles, no
             * call comes back to the dispatcher. The emitter's checked
             * blocks are only ever entered from it. */
            if (config != EMITTER) assert(many.stale == few.stale);
            else assert(many.stale > few.stale);
        }
}

/* A block that starts in read-only code and runs on into the writable page
 * checks its source; one that ends before it does not. */
static void test_reaching_writable(void)
{
    for (unsigned config = 0; config < CONFIGS; config++) {
        const uint32_t start = WRITABLE - 4;
        uint8_t code[16];
        size_t n = 0;

        reset_code();
        code[n++] = 0xe8; put32(code + n, (low + start) - (low + CODE + n + 4)); n += 4; /* call start */
        code[n++] = 0xc3;
        place(CODE, code, n);
        /* start: 4 nops, then (writable) mov eax, 5; ret */
        static const uint8_t tail[] = { 0x90, 0x90, 0x90, 0x90, 0xb8, 5, 0, 0, 0, 0xc3 };
        place(start, tail, sizeof(tail));
        Run r = run(config);
        assert(r.status == PW_OK && r.state.eip == SENTINEL && r.state.gpr[0] == 5);
        assert(r.verified == 1 && !r.verified_writable && !r.stale);
    }
}

int main(void)
{
    guest = mmap(NULL, SPAN, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
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
    test_rewritten_function();
    test_modloader_hook();
    test_reaching_writable();
    printf("x86 self-modifying code passed: rewritten functions, Mod Loader's hook toggle, "
           "blocks reaching writable code, read-only code unchecked\n");
    return 0;
}
