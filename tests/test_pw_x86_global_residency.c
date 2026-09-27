/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Global residency: the same guest GPRs in the same host registers in every
 * block, entered through pw_x86_run_block. */
#include "../src/pw_x86_engine.h"
#include "../src/pw_vm_posix.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

enum { BASE = 0x00400000u, GLOBAL = 0xfb };

typedef struct Source { const uint8_t *data; size_t bytes; } Source;

static int view(void *opaque, uint32_t pc, const uint8_t **data, size_t *bytes)
{
    const Source *s = opaque;
    if (pc < BASE || pc >= BASE + s->bytes) return PW_ERR_NOT_FOUND;
    *data = s->data + (pc - BASE);
    *bytes = s->bytes - (pc - BASE);
    return PW_OK;
}

static uint8_t *stack_page;

static void fresh_state(PwX86State *s, uint32_t eip)
{
    memset(s, 0, sizeof(*s));
    s->eip = eip;
    s->stack_low = (uint32_t)(uintptr_t)stack_page;
    s->stack_high = s->stack_low + 0x10000;
    s->gpr[4] = s->stack_high - 0x100;
    s->eflags = 0x2;
}

/* Run from eip until the guest returns to 0xdead0000 or a step fails;
 * returns the last step's status. */
static int run(const uint8_t *code, size_t bytes, uint8_t mask, PwX86State *s,
               PwX86Engine *keep)
{
    static PwX86CacheEntry entries[256];
    PwX86Engine engine;
    /* Static: a kept engine still points at its backend and source. */
    static PwVmBackend vm;
    static Source src;
    PwX86StepReport step;
    int status = PW_OK;

    src.data = code;
    src.bytes = bytes;
    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_engine_init(&engine, &vm, entries, 256, 1u << 20, 1, view, &src) == PW_OK);
    assert(pw_x86_engine_set_chaining(&engine, 1) == PW_OK);
    assert(pw_x86_engine_set_global_resident(&engine, mask) == PW_OK);
    *(uint32_t *)(uintptr_t)s->gpr[4] = 0xdead0000u;
    for (unsigned i = 0; i < 100000 && s->eip != 0xdead0000u; i++)
        if ((status = pw_x86_engine_step(&engine, s, &step)) != PW_OK) break;
    if (keep) *keep = engine;
    else assert(pw_x86_engine_destroy(&engine) == PW_OK);
    return status;
}

static void same_state(const PwX86State *a, const PwX86State *b)
{
    for (unsigned r = 0; r < 8; r++) assert(a->gpr[r] == b->gpr[r]);
    assert(a->eip == b->eip);
    assert((a->eflags & 0x8d5) == (b->eflags & 0x8d5));
}

static void test_mask_limits(void)
{
    static PwX86CacheEntry entries[4];
    PwX86Engine engine;
    PwVmBackend vm;
    Source src = { (const uint8_t *)"\xc3", 1 };

    assert(pw_x86_global_host(0xfb, 0) == 0 && pw_x86_global_host(0xfb, 1) == 1);
    assert(pw_x86_global_host(0xfb, 2) == -1 && pw_x86_global_host(0xfb, 3) == 2);
    assert(pw_x86_global_host(0xfb, 4) == 4 && pw_x86_global_host(0xfb, 7) == 7);
    assert(pw_x86_global_host(0xfe, 1) == 0 && pw_x86_global_host(0xff, 7) == -1);
    assert(pw_x86_global_host(0xfb, 8) == -1);
    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_engine_init(&engine, &vm, entries, 4, 65536, 1, view, &src) == PW_OK);
    assert(pw_x86_engine_set_global_resident(&engine, 0xff) == PW_ERR_PRECONDITION);
    assert(pw_x86_engine_set_global_resident(&engine, 0x7f) == PW_OK);
    assert(pw_x86_engine_set_global_resident(NULL, 0x7f) == PW_ERR_PRECONDITION);
    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

/* Every block without a helper gets the one contract, even a tiny one, and
 * a block with a string helper keeps an empty one. */
static void test_contracts(void)
{
    static const uint8_t tiny[] = { 0x40, 0xc3 };                 /* inc eax; ret */
    static const uint8_t helper[] = { 0xf3, 0xa4, 0xc3 };         /* rep movsb; ret */
    const PwX86TranslateOptions global = { .residency_enabled = 1, .lazy_flags_enabled = 1,
                                           .global_resident = GLOBAL };
    const PwX86TranslateOptions off = { .residency_enabled = 0, .lazy_flags_enabled = 1,
                                        .global_resident = GLOBAL };
    uint8_t out[4096];
    PwX86Block block;

    assert(pw_x86_translate_opts(tiny, sizeof(tiny), BASE, out, sizeof(out), &block, &global) == PW_OK);
    assert(block.entry_contract.resident_mask == GLOBAL);
    for (unsigned g = 0; g < 8; g++)
        assert(block.entry_contract.guest_to_host[g] == pw_x86_global_host(GLOBAL, g));
    assert(pw_x86_translate_opts(helper, sizeof(helper), BASE, out, sizeof(out), &block, &global) == PW_OK);
    assert(block.entry_contract.resident_mask == 0);
    /* Residency off wins over the mask. */
    assert(pw_x86_translate_opts(tiny, sizeof(tiny), BASE, out, sizeof(out), &block, &off) == PW_OK);
    assert(block.entry_contract.resident_mask == 0);
}

/* A loop across linked blocks and a call:
 * the result matches the run without residency, and the loop links
 * directly rather than through reconciliation. */
static void test_linked_values(void)
{
    static const uint8_t code[] = {
        0xb9, 0x40, 0x00, 0x00, 0x00,   /* 00 mov ecx, 64 */
        0x31, 0xc0,                     /* 05 xor eax, eax */
        0xbb, 0x03, 0x00, 0x00, 0x00,   /* 07 mov ebx, 3 */
        0x01, 0xc8,                     /* 0c L: add eax, ecx */
        0x0f, 0xaf, 0xc3,               /* 0e imul eax, ebx */
        0x89, 0xc6,                     /* 11 mov esi, eax */
        0x49,                           /* 13 dec ecx */
        0x75, 0xf6,                     /* 14 jnz L */
        0xe8, 0x03, 0x00, 0x00, 0x00,   /* 16 call F */
        0x89, 0xda,                     /* 1b mov edx, ebx */
        0xc3,                           /* 1d ret */
        0x8d, 0x3c, 0x36,               /* 1e F: lea edi, [esi+esi] */
        0xc3,                           /* 21 ret */
    };
    PwX86State with, without;
    PwX86Engine engine;
    uint64_t direct = 0;

    fresh_state(&with, BASE);
    fresh_state(&without, BASE);
    assert(run(code, sizeof(code), 0, &without, NULL) == PW_OK);
    assert(run(code, sizeof(code), GLOBAL, &with, &engine) == PW_OK);
    same_state(&with, &without);
    for (uint32_t i = 0; i < engine.cache.capacity; i++) {
        const PwX86CacheEntry *e = &engine.cache.entries[i];
        if (!e->used || e->guest_pc != BASE + 0x0c) continue;
        assert(e->link_slots[0].is_linked && !e->link_slots[0].is_reconciled);
        direct++;
    }
    assert(direct == 1);
    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

/* A fault after the block changed resident registers stops with the
 * values those instructions produced and EIP at the faulting one. */
static void test_fault_state(void)
{
    static const uint8_t code[] = {
        0xb8, 0x05, 0x00, 0x00, 0x00,   /* mov eax, 5 */
        0xbd, 0x07, 0x00, 0x00, 0x00,   /* mov ebp, 7 */
        0x8d, 0x7c, 0x28, 0x01,         /* lea edi, [eax+ebp+1] */
        0x89, 0x05, 0x10, 0x00, 0x00, 0x00, /* mov [0x10], eax: refused */
        0x40,                           /* inc eax */
        0xc3,
    };
    PwX86State s;

    fresh_state(&s, BASE);
    assert(run(code, sizeof(code), GLOBAL, &s, NULL) != PW_OK);
    assert(s.gpr[0] == 5 && s.gpr[5] == 7 && s.gpr[7] == 13);
    assert(s.eip == BASE + 14);
    assert(s.fault_address == 0x10);
}

/* rbx, rbp and r12-r15 hold the caller's values across a block that keeps
 * guest values in them. */
static void test_trampoline_saves(void)
{
    static const uint8_t code[] = {
        0xb8, 1, 0, 0, 0, 0xb9, 2, 0, 0, 0, 0xbb, 3, 0, 0, 0,
        0xbd, 4, 0, 0, 0, 0xbe, 5, 0, 0, 0, 0xbf, 6, 0, 0, 0, 0xc3,
    };
    const PwX86TranslateOptions global = { .residency_enabled = 1, .lazy_flags_enabled = 1,
                                           .global_resident = 0x7f };
    PwVmBackend vm;
    PwVmRegion region;
    PwX86Block block;
    PwX86State s;
    uint64_t saved[6] = { 0 };
    int status;

    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(vm.reserve(vm.context, 1u << 16, vm.page_bytes, &region) == PW_OK);
    assert(vm.commit(vm.context, &region, 0, region.bytes, PW_PROT_READ | PW_PROT_WRITE) == PW_OK);
    assert(pw_x86_translate_opts(code, sizeof(code), BASE, region.write_base, region.bytes, &block,
                                 &global) == PW_OK);
    assert(block.entry_contract.resident_mask == 0x7f);
    assert(vm.protect(vm.context, &region, 0, region.bytes, PW_PROT_READ | PW_PROT_EXEC) == PW_OK);
    fresh_state(&s, BASE);
    *(uint32_t *)(uintptr_t)s.gpr[4] = 0xdead0000u;
    {
        void *state_arg = &s, *entry_arg = region.exec_base, *out_arg = saved;
        __asm__ volatile(
            "mov %%rsp, %%rax\n sub $256, %%rsp\n and $-16, %%rsp\n"
            "push %%rax\n push %%rdx\n"          /* rsp and out; rsp stays 16-aligned */
            "push %%rbx\n push %%rbp\n push %%r12\n push %%r13\n push %%r14\n push %%r15\n"
            "mov $0x1111, %%rbx\n mov $0x2222, %%rbp\n mov $0x3333, %%r12\n"
            "mov $0x4444, %%r13\n mov $0x5555, %%r14\n mov $0x6666, %%r15\n"
            "call pw_x86_run_block\n"
            "mov %%eax, %%ecx\n mov 48(%%rsp), %%rax\n"
            "mov %%rbx, 0(%%rax)\n mov %%rbp, 8(%%rax)\n mov %%r12, 16(%%rax)\n"
            "mov %%r13, 24(%%rax)\n mov %%r14, 32(%%rax)\n mov %%r15, 40(%%rax)\n"
            "pop %%r15\n pop %%r14\n pop %%r13\n pop %%r12\n pop %%rbp\n pop %%rbx\n"
            "pop %%rdx\n pop %%rsp\n"
            : "=c"(status), "+D"(state_arg), "+S"(entry_arg), "+d"(out_arg)
            :
            : "rax", "r8", "r9", "r10", "r11", "memory", "cc");
    }
    assert(status == 0);
    assert(saved[0] == 0x1111 && saved[1] == 0x2222 && saved[2] == 0x3333);
    assert(saved[3] == 0x4444 && saved[4] == 0x5555 && saved[5] == 0x6666);
    assert(s.gpr[0] == 1 && s.gpr[3] == 3 && s.gpr[5] == 4 && s.gpr[6] == 5 && s.gpr[7] == 6);
    assert(s.eip == 0xdead0000u);
    assert(vm.release(vm.context, &region) == PW_OK);
}

int main(void)
{
    stack_page = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT,
                      -1, 0);
    assert(stack_page != MAP_FAILED);
    test_mask_limits();
    test_contracts();
    test_linked_values();
    test_fault_state();
    test_trampoline_saves();
    printf("global residency passed: host slots, contracts, linked values, fault state, "
           "callee-saved registers\n");
    return 0;
}
