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
static unsigned host_call_stack;
static _Alignas(16) uint8_t call_memory[0x4000];

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
    if (host_call_stack) {
        assert(pw_x86_engine_set_unbounded_chains(&engine, 1) == PW_OK);
        assert(pw_x86_engine_set_call_stack(&engine, call_memory, sizeof(call_memory)) == PW_OK);
    }
    current = &engine;
    redirected = 0;
    r.status = PW_OK;
    for (unsigned i = 0; i < 1000 && r.state.eip != 0xdead0000u; i++)
        if ((r.status = pw_x86_engine_step(&engine, &r.state, &step)) != PW_OK) break;
    current = NULL;
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

static void test_multiple_fault_paths(void)
{
    /* Fifteen loads in one block, each followed by ADC so the incoming
     * flags must survive its guard. Fault the first, middle and final
     * accesses in turn; prior loads/ADCs must have updated the guest state. */
    uint8_t program[2 + 15 * 8 + 1];
    uint32_t address = low + DATA, value = 7;
    memcpy(guest + DATA, &value, sizeof(value));
    program[0]=0x31;program[1]=0xdb; /* xor ebx,ebx */
    for (unsigned k=0;k<15;k++) {
        unsigned at=2+8*k;
        program[at]=0x8b;program[at+1]=0x05; /* mov eax,[absolute] */
        memcpy(program+at+2,&address,4);
        program[at+6]=0x11;program[at+7]=0xc3; /* adc ebx,eax */
    }
    program[sizeof(program)-1]=0xc3;
    for (unsigned stack=0;stack<2;stack++) {
        host_call_stack=stack;
        for (unsigned site=0;site<15;site++) {
            uint32_t invalid=0x40;
            memcpy(program+2+8*site+2,&invalid,4);
            compare("multiple cold paths",program,sizeof(program),2+8*site,invalid,0,-1);
            Run marked=run(program,sizeof(program),1);
            assert(marked.state.gpr[3]==site*value);
            memcpy(program+2+8*site+2,&address,4);
        }
    }
    host_call_stack=0;
}

/* div and idiv with markers: a divisor read from the null page faults
 * through its marked load in every width (the flags still the guest's, as
 * the check comes after the load), and a divide error, which is no host
 * fault, stops both modes alike without a redirect. */
static void test_divide(void)
{
    /* xor ebx, ebx; cmp ecx, edx; div dword [ebx+8]; ret */
    static const uint8_t wide[] = { 0x31, 0xdb, 0x39, 0xd1, 0xf7, 0x73, 0x08, 0xc3 };
    /* xor ebx, ebx; cmp ecx, edx; idiv byte [ebx+8]; ret */
    static const uint8_t narrow[] = { 0x31, 0xdb, 0x39, 0xd1, 0xf6, 0x7b, 0x08, 0xc3 };
    /* xor ebx, ebx; cmp ecx, edx; div word [ebx+8]; ret */
    static const uint8_t word[] = { 0x31, 0xdb, 0x39, 0xd1, 0x66, 0xf7, 0x73, 0x08, 0xc3 };
    /* push 0; mov eax, 7; cmp ecx, edx; idiv dword [esp]; ret */
    static const uint8_t zero[] = { 0x6a, 0x00, 0xb8, 7, 0, 0, 0, 0x39, 0xd1, 0xf7, 0x3c, 0x24, 0xc3 };
    /* push 7; xor edx, edx; mov eax, 100; div dword [esp]; pop ecx; div cl; ret */
    static const uint8_t fine[] = { 0x6a, 0x07, 0x31, 0xd2, 0xb8, 100, 0, 0, 0, 0xf7, 0x34, 0x24, 0x59,
                                    0xf6, 0xf1, 0xc3 };
    Run guarded, marked;

    compare("div load", wide, sizeof(wide), 4, 8, 0, -1);
    compare("idiv byte load", narrow, sizeof(narrow), 4, 8, 0, -1);
    compare("div word load", word, sizeof(word), 4, 8, 0, -1);
    guarded = run(zero, sizeof(zero), 0);
    marked = run(zero, sizeof(zero), 1);
    assert(guarded.status == PW_ERR_VM && guarded.state.eip == low + CODE + 9);
    assert(marked.status == guarded.status && marked.state.eip == guarded.state.eip && !marked.redirected);
    for (unsigned g = 0; g < 8; g++) assert(marked.state.gpr[g] == guarded.state.gpr[g]);
    assert(marked.state.gpr[0] == 7 && (marked.state.eflags & 0x8d5) == (guarded.state.eflags & 0x8d5));
    guarded = run(fine, sizeof(fine), 0);
    marked = run(fine, sizeof(fine), 1);
    assert(guarded.status == PW_OK && marked.status == PW_OK && !marked.redirected);
    for (unsigned g = 0; g < 8; g++) assert(marked.state.gpr[g] == guarded.state.gpr[g]);
    assert(marked.state.gpr[0] == (uint32_t)((100 / 7) % 7 << 8 | (100 / 7) / 7));
}

/* ah-bh beside a memory operand on the null page, copied as they are
 * (ebx-based) or through r10b (edi- and esp-based), as source and
 * destination; and bt m,r whose unit, not its base, is on the null page,
 * with positive, negative and 16-bit offsets. Each faults at its own
 * access with the high byte's register untouched, as the guard reports. */
static void test_high_bytes_and_bit_strings(void)
{
    /* xor ebx, ebx; cmp ecx, edx; mov bh, [ebx+8]; ret */
    static const uint8_t load[] = { 0x31, 0xdb, 0x39, 0xd1, 0x8a, 0x7b, 0x08, 0xc3 };
    /* xor edi, edi; cmp ecx, edx; mov [edi+8], ah; ret */
    static const uint8_t store[] = { 0x31, 0xff, 0x39, 0xd1, 0x88, 0x67, 0x08, 0xc3 };
    /* xor edi, edi; cmp ecx, edx; adc ch, [edi+0x10]; ret */
    static const uint8_t modify[] = { 0x31, 0xff, 0x39, 0xd1, 0x12, 0x6f, 0x10, 0xc3 };
    /* xor esp, esp; cmp ecx, edx; xchg [esp+0x20], dh; ret */
    static const uint8_t swap[] = { 0x31, 0xe4, 0x39, 0xd1, 0x86, 0x74, 0x24, 0x20, 0xc3 };
    /* xor ebx, ebx; mov eax, 64; cmp ecx, edx; bts [ebx], eax; ret */
    static const uint8_t bts[] = { 0x31, 0xdb, 0xb8, 64, 0, 0, 0, 0x39, 0xd1, 0x0f, 0xab, 0x03, 0xc3 };
    /* mov ebx, 0x100; mov eax, -1024; cmp ecx, edx; bt [ebx], eax; ret */
    static const uint8_t back[] = { 0xbb, 0, 1, 0, 0, 0xb8, 0x00, 0xfc, 0xff, 0xff, 0x39, 0xd1,
                                    0x0f, 0xa3, 0x03, 0xc3 };
    /* mov edi, 0x800; mov eax, 0xabcdfc00; cmp ecx, edx; btr word [edi], ax; ret */
    static const uint8_t word[] = { 0xbf, 0, 8, 0, 0, 0xb8, 0x00, 0xfc, 0xcd, 0xab, 0x39, 0xd1,
                                    0x66, 0x0f, 0xb3, 0x07, 0xc3 };
    Run marked;

    compare("high byte load", load, sizeof(load), 4, 8, 0, -1);
    compare("high byte store", store, sizeof(store), 4, 8, 1, -1);
    compare("high byte adc", modify, sizeof(modify), 4, 0x10, 0, -1);
    marked = run(modify, sizeof(modify), 1);
    assert(marked.state.gpr[1] == 0x04040404u);
    compare("high byte xchg", swap, sizeof(swap), 4, 0x20, 2, -1);
    marked = run(swap, sizeof(swap), 1);
    assert(marked.state.gpr[2] == 0x05050505u);
    compare("bts past the base", bts, sizeof(bts), 9, 8, 2, -1);
    compare("bt before the base", back, sizeof(back), 12, 0x80, 0, -1);
    compare("btr word before the base", word, sizeof(word), 12, 0x780, 2, -1);
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

/* New implicit-address reads must recover the same guest state through an
 * actual host fault as through the flat guard, including guest DF. */
static void test_main_instruction_faults(void)
{
    uint8_t fs_source[]={0x39,0xd1,0xfd,0x64,0xff,0x35,0x40,0,0,0,0x31,0xc0,0xc3};
    uint8_t fs_stack[]={0x39,0xd1,0xfd,0xbc,0x20,0,0,0,
                        0x64,0xff,0x35,0,0,0,0,0x31,0xc0,0xc3};
    uint32_t address=low+DATA,value=0xdead1234;
    memcpy(fs_stack+11,&address,4);memcpy(guest+DATA,&value,4);
    uint8_t xlat[]={0xbb,0x40,0,0,0,0xb8,3,0xcc,0xbb,0xaa,
                    0x39,0xd1,0xfd,0xd7,0x31,0xc0,0xc3};
    uint8_t fs_xlat[]={0xbb,0x40,0,0,0,0xb8,3,0xcc,0xbb,0xaa,
                       0x39,0xd1,0xfd,0x64,0xd7,0x31,0xc0,0xc3};
    const uint8_t *programs[]={fs_source,fs_stack,xlat,fs_xlat};
    const size_t sizes[]={sizeof(fs_source),sizeof(fs_stack),sizeof(xlat),sizeof(fs_xlat)};
    const uint32_t offsets[]={3,8,13,13},addresses[]={0x40,0x1c,0x43,0x43};
    for(unsigned stack=0;stack<2;++stack) {
        host_call_stack=stack;
        for(unsigned i=0;i<4;++i) {
            compare("FS push/XLAT",programs[i],sizes[i],offsets[i],addresses[i],i==1,-1);
            Run guarded=run(programs[i],sizes[i],0),marked=run(programs[i],sizes[i],1);
            assert((guarded.state.eflags&0x400) && (marked.state.eflags&0x400));
            assert(marked.state.fault_width==(i<2?4:1));
            if(i==1) assert(marked.state.gpr[4]==0x20);
            if(i>=2) assert(marked.state.gpr[0]==0xaabbcc03);
            uint64_t host_flags;__asm__ volatile("pushfq;popq %0":"=r"(host_flags));
            assert(!(host_flags&0x400));
        }
    }
    host_call_stack=0;
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
    test_memory_forms();
    test_engine_lookup();
    test_profile_capacity();
    test_fault_table();
    test_multiple_fault_paths();
    test_divide();
    test_high_bytes_and_bit_strings();
    test_main_instruction_faults();
    printf("fault markers passed: loads, stores, a locked read-modify-write, push and pop faulting "
           "on the null page report the guard's EIP, registers, flags and fault; every copied addressing "
           "and stack form matches the guard; the engine finds each access's path and nothing else; div's divisor load faults like any other, "
           "its divide error without a host fault; high bytes beside memory and bit strings fault at their own access\n");
    return 0;
}
