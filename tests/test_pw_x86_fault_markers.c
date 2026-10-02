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
static unsigned redirected, native_fp;

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
    if(native_fp) { pw_guest_fp_init(&r.state.fp); memset(r.state.fp.xmm,0x5a,sizeof(r.state.fp.xmm)); }
    memcpy(guest + STACK_TOP, &(uint32_t){ 0xdead0000u }, 4);
    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_engine_init(&engine, &vm, entries, 512, 1u << 20, 1, view, NULL) == PW_OK);
    assert(pw_x86_engine_set_chaining(&engine, 1) == PW_OK);
    assert(pw_x86_engine_set_indirect(&engine, 1) == PW_OK);
    assert(pw_x86_engine_set_counters(&engine, 0) == PW_OK);
    assert(pw_x86_engine_set_flat_memory(&engine, low, low + SPAN) == PW_OK);
    assert(pw_x86_engine_set_reencode(&engine, 1) == PW_OK);
    assert(pw_x86_engine_set_fault_markers(&engine, markers) == PW_OK);
    assert(pw_x86_engine_set_native_fp(&engine,native_fp)==PW_OK);
    current = &engine;
    redirected = 0;
    r.status = PW_OK;
    for (unsigned i = 0; i < 1000 && r.state.eip != 0xdead0000u; i++)
        if ((r.status = pw_x86_engine_step(&engine, &r.state, &step)) != PW_OK) break;
    current = NULL;
    pw_x86_engine_fp_sync(&engine,&r.state);
    r.reencoded = engine.reencoded_blocks;
    r.redirected = redirected;
    if (engine.fault_markers) {
        PwX86HotspotProfile profile;
        memset(&profile, 0, sizeof(profile));
        uintptr_t base = (uintptr_t)engine.code.exec_base;
        assert(!pw_x86_engine_host_block(&engine, base - 1));
        assert(!pw_x86_engine_host_block(&engine, base + engine.code.bytes));
        for (unsigned i = 0; i < engine.cache.capacity; i++) {
            const PwX86CacheEntry *e = &entries[i];
            if (!e->used || e->generation != engine.cache.generation) continue;
            assert(pw_x86_engine_host_block(&engine, base + e->code_offset) == e);
            pw_x86_engine_sample(&engine, base + e->code_offset, &profile);
            pw_x86_engine_sample(&engine, base + e->code_offset + e->code_bytes - 1, &profile);
            assert(pw_x86_engine_host_block(&engine, base + e->code_offset + e->code_bytes - 1) == e);
        }
        {
            uint64_t counted = 0;
            for (unsigned i = 0; i < PW_X86_HOTSPOT_SLOTS; i++) {
                const PwX86Hotspot *row = &profile.slots[i];
                counted += row->samples;
                assert(row->samples == row->entry + row->body + row->exit + row->emitted);
            }
            assert(counted == profile.samples && counted && !profile.overflow && !profile.stubs);
            pw_x86_engine_sample(&engine, base - 1, &profile);
            assert(profile.outside == 1);
        }
        assert(pw_x86_engine_reset(&engine, engine.cache.generation + 1) == PW_OK);
        pw_x86_engine_sample(&engine, base, &profile);
        assert(profile.stubs == 1);
        assert(!pw_x86_engine_host_block(&engine, base));
    }
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

/* A fault at the first SIMD access and one after SIMD changed the image:
 * the lazy prepare path must not change guest flags or lose guest FP state. */
static void test_lazy_fp_faults(void)
{
    static const uint8_t first[] = {
        0xb8,0xff,0xff,0xff,0xff, 0x05,1,0,0,0,
        0x0f,0x10,0x05,0x10,0,0,0, /* movups xmm0,[0x10] */
        0x0f,0x92,0xc2,0xc3,
    };
    static const uint8_t later[] = {
        0x66,0x0f,0xef,0xc0, /* pxor xmm0,xmm0 */
        0xb8,0xff,0xff,0xff,0xff, 0x05,1,0,0,0,
        0x89,0x05,0x10,0,0,0,0x0f,0x92,0xc2,0xc3,
    };
    native_fp=1;
    compare("lazy first SIMD fault",first,sizeof(first),10,0x10,0,0x55);
    compare("lazy post SIMD fault",later,sizeof(later),14,0x10,1,0x55);
    for(unsigned k=0;k<2;k++) {
        Run a=run(k?later:first,k?sizeof(later):sizeof(first),0);
        Run b=run(k?later:first,k?sizeof(later):sizeof(first),1);
        assert(a.state.native_fp_active && b.state.native_fp_active);
        assert(a.state.fp.x87_control==b.state.fp.x87_control && a.state.fp.mxcsr==b.state.fp.mxcsr);
        assert(!memcmp(a.state.fp.xmm,b.state.fp.xmm,sizeof(a.state.fp.xmm)));
        for(unsigned i=0;i<16;i++) assert(b.state.fp.xmm[0][i]==(k?0:0x5a));
    }
    native_fp=0;
}

/* The program test_memory_forms runs: every addressing and stack form. */
static uint8_t forms[] = {
        0x8d, 0xb4, 0x24, 0x00, 0x80, 0xff, 0xff, 0x8d, 0xbc, 0x24, 0x00, 0x90,
        0xff, 0xff, 0x8d, 0xac, 0x24, 0x00, 0xa0, 0xff, 0xff, 0xb8, 0x44, 0x33,
        0x22, 0x11, 0xb9, 0x03, 0x00, 0x00, 0x00, 0x89, 0x06, 0x89, 0x0c, 0x8f,
        0x89, 0x44, 0xcf, 0x40, 0x01, 0x45, 0x08, 0x29, 0x4d, 0xfc, 0x89, 0x85,
        0x00, 0x10, 0x00, 0x00, 0x89, 0x44, 0x24, 0xf0, 0x8b, 0x54, 0x24, 0xf0,
        0x89, 0x0c, 0x8d, 0x78, 0x56, 0x34, 0x12, 0x89, 0x0d, 0x78, 0x56, 0x34,
        0x12, 0x0f, 0xb6, 0x1c, 0x8f, 0x8a, 0x5e, 0x01, 0x88, 0x4e, 0x02, 0x66,
        0x89, 0x46, 0x04, 0xf0, 0x0f, 0xc1, 0x0e, 0xf0, 0x0f, 0xb1, 0x0f, 0xb9,
        0x03, 0x00, 0x00, 0x00, 0x33, 0x06, 0x39, 0x14, 0x8f, 0x13, 0x06, 0x6b,
        0x1e, 0x07, 0xf7, 0x06, 0x00, 0x00, 0x00, 0x80, 0x81, 0x17, 0x45, 0x23,
        0x01, 0x00, 0xc7, 0x47, 0x20, 0x55, 0x00, 0x00, 0x00, 0xff, 0x36, 0x5a,
        0x1b, 0x57, 0x04, 0x8b, 0x07, 0xbf, 0x10, 0x00, 0x00, 0x00, 0x8b, 0x0c,
        0xbe, 0x03, 0x44, 0x7d, 0xf8, 0x88, 0x94, 0x3e, 0x00, 0x01, 0x00, 0x00,
        0x56, 0x68, 0x34, 0x12, 0x00, 0x00, 0x54, 0x58, 0xff, 0x36, 0x5b, 0x59,
        0x5a, 0x55, 0x89, 0xe5, 0x6a, 0x07, 0x6a, 0x08, 0xc9, 0x6a, 0x09, 0xe8,
        0x02, 0x00, 0x00, 0x00, 0xeb, 0x07, 0x03, 0x44, 0x24, 0x04, 0xc2, 0x04,
        0x00, 0x54, 0x5c, 0xc3,
};

/* forms with its absolute addresses pointed into the guest. */
static void patch_forms(uint8_t *code)
{
    const uint32_t absolute = low + 0x3c000;
    memcpy(code + 0x3f, &absolute, 4);  /* mov %ecx, abs(,%ecx,4) */
    memcpy(code + 0x45, &absolute, 4);  /* mov %ecx, abs */
}

/* Every addressing form the re-encoder copies with fault markers (esp and
 * edi as base or index, no base, an absolute address, disp8 and disp32,
 * 8- and 16-bit operands, lock, immediates, flags read across accesses) and
 * every stack form (push of a register, esp, memory and an immediate, pop
 * of a register and of esp, leave, call, ret and ret imm16) runs exactly as
 * with the guard: same registers, flags and memory. */
static void test_memory_forms(void)
{
    uint8_t code[sizeof(forms)];
    uint8_t after[2][0x10000];
    Run r[2];

    memcpy(code, forms, sizeof(code));
    patch_forms(code);
    for (unsigned m = 0; m < 2; m++) {
        for (unsigned i = 0; i < 0x10000; i++) guest[0x30000 + i] = (uint8_t)(i * 7 + 1);
        r[m] = run(code, sizeof(code), m);
        memcpy(after[m], guest + 0x30000, sizeof(after[m]));
    }
    assert(r[0].status == PW_OK && r[1].status == PW_OK && r[0].reencoded && r[1].reencoded);
    assert(!r[1].redirected && r[0].state.eip == 0xdead0000u && r[1].state.eip == 0xdead0000u);
    for (unsigned g = 0; g < 8; g++) {
        if (r[0].state.gpr[g] != r[1].state.gpr[g])
            fprintf(stderr, "memory forms: gpr%u %08x != %08x\n", g, r[1].state.gpr[g], r[0].state.gpr[g]);
        assert(r[0].state.gpr[g] == r[1].state.gpr[g]);
    }
    assert((r[0].state.eflags & 0x8d5) == (r[1].state.eflags & 0x8d5));
    assert(!memcmp(after[0], after[1], sizeof(after[0])));
}

/* The engine's lookup from a host address to a block's refused-access path:
 * every listed access finds its path, any other address in a block (its
 * canonical entry, found by walking the blocks of a granule) or past the
 * last block finds none, and a reset forgets the blocks. Also the call
 * stack's preconditions and the return stub's. */
static void test_engine_lookup(void)
{
    static PwX86CacheEntry entries[512];
    static PwVmBackend vm;
    PwX86Engine engine;
    PwX86StepReport step;
    PwX86State state;
    unsigned sites = 0, walked = 0;
    uint8_t stub[512];
    PwX86TranslateOptions options;

    /* Several blocks: the memory forms program, translated with markers. */
    memset(guest + CODE, 0xcc, DATA - CODE);
    memcpy(guest + CODE, forms, sizeof(forms));
    patch_forms(guest + CODE);
    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_engine_init(&engine, &vm, entries, 512, 1u << 20, 1, view, NULL) == PW_OK);
    assert(pw_x86_engine_set_chaining(&engine, 1) == PW_OK);
    assert(pw_x86_engine_set_indirect(&engine, 1) == PW_OK);
    assert(pw_x86_engine_set_counters(&engine, 0) == PW_OK);
    assert(pw_x86_engine_set_flat_memory(&engine, low, low + SPAN) == PW_OK);
    assert(pw_x86_engine_set_reencode(&engine, 1) == PW_OK);
    /* The call stack needs unbounded chains; NULL turns it off. */
    assert(pw_x86_engine_set_call_stack(&engine, guest, 0x4000) == PW_ERR_PRECONDITION);
    assert(pw_x86_engine_set_call_stack(&engine, NULL, 0) == PW_OK);
    assert(pw_x86_engine_set_fault_markers(&engine, 1) == PW_OK);
    assert(!pw_x86_engine_fault_redirect(&engine, (uintptr_t)engine.code.exec_base));  /* nothing yet */
    memset(&state, 0, sizeof(state));
    state.eip = low + CODE;
    state.stack_low = low;
    state.stack_high = low + SPAN;
    state.memory_count = 1;
    state.memory[0].low = low;
    state.memory[0].high = (uint64_t)low + SPAN;
    state.memory[0].permissions = PW_X86_READ | PW_X86_WRITE;
    state.gpr[4] = low + STACK_TOP;
    state.eflags = 0x2;
    memcpy(guest + STACK_TOP, &(uint32_t){ 0xdead0000u }, 4);
    current = &engine;
    for (unsigned i = 0; i < 1000 && state.eip != 0xdead0000u; i++)
        assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    current = NULL;
    assert(state.eip == 0xdead0000u);
    for (uint32_t i = 0; i < engine.cache.capacity; i++) {
        const PwX86CacheEntry *e = &engine.cache.entries[i];
        const uint8_t *start = (const uint8_t *)engine.code.exec_base + e->code_offset;
        if (!e->used || !e->fault_table_offset) continue;
        {
            const unsigned count = start[e->fault_table_offset] | start[e->fault_table_offset + 1] << 8;
            for (unsigned k = 0; k < count; k++) {
                const uint8_t *row = start + e->fault_table_offset + 2 + 4 * k;
                const size_t site = row[0] | row[1] << 8, path = row[2] | row[3] << 8;
                assert(pw_x86_engine_fault_redirect(&engine, (uintptr_t)start + site) == (uintptr_t)start + path);
                sites++;
            }
        }
        /* The canonical entry is never an access. */
        assert(!pw_x86_engine_fault_redirect(&engine, (uintptr_t)start));
        assert(!pw_x86_engine_fault_redirect(&engine, (uintptr_t)start + 1));
        if (engine.block_map[e->code_offset / PW_X86_ENGINE_FAULT_GRANULE] != i + 1) walked++;
    }
    assert(sites && walked);
    /* Past the last block, and outside the arena. */
    assert(!pw_x86_engine_fault_redirect(&engine, (uintptr_t)engine.code.exec_base + engine.cache.cursor + 1));
    assert(!pw_x86_engine_fault_redirect(&engine, (uintptr_t)engine.code.exec_base + engine.code.bytes));
    assert(pw_x86_engine_reset(&engine, 2) == PW_OK);
    assert(!engine.block_map[0] && !engine.last_published);
    assert(pw_x86_engine_destroy(&engine) == PW_OK);

    /* The return stub needs a call stack and its tables. */
    memset(&options, 0, sizeof(options));
    assert(!pw_x86_reencode_return_stub(stub, sizeof(stub), &options));
    assert(!pw_x86_reencode_return_stub(NULL, sizeof(stub), &options));
}

static void test_profile_capacity(void)
{
    PwX86Engine engine;
    PwX86CacheEntry entry;
    PwX86HotspotProfile profile;
    uint32_t map = 1;
    memset(&engine, 0, sizeof(engine));
    memset(&entry, 0, sizeof(entry));
    memset(&profile, 0, sizeof(profile));
    engine.code.exec_base = (void *)(uintptr_t)0x10000;
    engine.code.bytes = 1;
    engine.block_map = &map;
    engine.cache.entries = &entry;
    engine.cache.capacity = engine.cache.generation = 1;
    entry.used = entry.generation = entry.code_bytes = 1;
    entry.guest_pc = 0x2000;
    pw_x86_engine_sample(&engine, 0x10000, &profile);
    entry.guest_pc += PW_X86_HOTSPOT_SLOTS; /* Same hash bucket. */
    pw_x86_engine_sample(&engine, 0x10000, &profile);
    assert(profile.samples == 2 && !profile.overflow);
    assert(profile.slots[0].samples == 1 && profile.slots[1].samples == 1);
    for(unsigned i = 0; i < PW_X86_HOTSPOT_SLOTS; i++) {
        profile.slots[i].guest_pc = i;
        profile.slots[i].samples = 1;
    }
    pw_x86_engine_sample(&engine, 0x10000, &profile);
    assert(profile.samples == 3 && profile.overflow == 1);
    for(unsigned i = 0; i < PW_X86_HOTSPOT_SLOTS; i++)
        assert(profile.slots[i].guest_pc == i && profile.slots[i].samples == 1);
}

static void test_fault_table(void)
{
    /* Three rows: sites 0x10, 0x30 and 0x2345 with their paths. */
    static const uint8_t code[64] = {
        [40] = 3, 0,
        0x10, 0x00, 0x80, 0x00,
        0x30, 0x00, 0x90, 0x01,
        0x45, 0x23, 0x00, 0x20,
    };

    assert(pw_x86_fault_table_path(code, 40, 0x10) == 0x80);
    assert(pw_x86_fault_table_path(code, 40, 0x30) == 0x190);
    assert(pw_x86_fault_table_path(code, 40, 0x2345) == 0x2000);
    assert(!pw_x86_fault_table_path(code, 40, 0x11));      /* not an access */
    assert(!pw_x86_fault_table_path(code, 40, 0));
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
    test_lazy_fp_faults();
    test_memory_forms();
    test_engine_lookup();
    test_profile_capacity();
    test_fault_table();
    printf("fault markers passed: loads, stores, a locked read-modify-write, push and pop faulting "
           "on the null page report the guard's EIP, registers, flags and fault; every copied addressing "
           "and stack form matches the guard; the engine finds each access's path and nothing else\n");
    return 0;
}
