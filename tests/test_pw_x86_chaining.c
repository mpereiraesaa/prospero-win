/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_x86_engine.h"
#include "../src/pw_vm_posix.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct TestSource {
    uint32_t base;
    const uint8_t *data;
    size_t bytes;
} TestSource;

static int test_source_view(void *opaque, uint32_t pc, const uint8_t **data, size_t *bytes)
{
    TestSource *s = (TestSource *)opaque;
    if (pc < s->base || (uint64_t)pc >= s->base + s->bytes) return PW_ERR_NOT_FOUND;
    size_t offset = pc - s->base;
    *data = s->data + offset;
    *bytes = s->bytes - offset;
    return PW_OK;
}

/* 1. Two-block and multi-block unconditional chains */
static void test_unconditional_chains(void)
{
    /*
     * 0x1000: mov eax, 10; jmp 0x1010
     * 0x1010: add eax, 20; jmp 0x1020
     * 0x1020: add eax, 30; ret
     */
    uint8_t code[64];
    memset(code, 0x90, sizeof(code));
    /* 0x1000 (offset 0): b8 0a 00 00 00 (mov eax, 10); e9 06 00 00 00 (jmp 0x1010) */
    code[0] = 0xb8; code[1] = 10; code[2] = 0; code[3] = 0; code[4] = 0;
    code[5] = 0xe9; code[6] = 6; code[7] = 0; code[8] = 0; code[9] = 0;
    /* 0x1010 (offset 16): 83 c0 14 (add eax, 20); e9 08 00 00 00 (jmp 0x1020) */
    code[16] = 0x83; code[17] = 0xc0; code[18] = 20;
    code[19] = 0xe9; code[20] = 8; code[21] = 0; code[22] = 0; code[23] = 0;
    /* 0x1020 (offset 32): 83 c0 1e (add eax, 30); c3 (ret) */
    code[32] = 0x83; code[33] = 0xc0; code[34] = 30;
    code[35] = 0xc3;

    TestSource src = {0x1000, code, sizeof(code)};
    PwVmBackend vm;
    PwX86Engine engine;
    PwX86CacheEntry entries[16];
    PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
    state.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999; /* return address */

    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_engine_init(&engine, &vm, entries, 16, 65536, 1, test_source_view, &src) == PW_OK);
    assert(pw_x86_engine_set_chaining(&engine, 1) == PW_OK);

    /* First run: compile blocks on demand */
    PwX86StepReport step;
    /* Step 1: compiles 0x1000, runs it, exits unlinked to 0x1010 */
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.eip == 0x1010);
    assert(state.gpr[0] == 10);

    /* Step 2: compiles 0x1010, runs it, exits unlinked to 0x1020 */
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.eip == 0x1020);
    assert(state.gpr[0] == 30);

    /* Step 3: compiles 0x1020, runs it, ret exits cleanly */
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.eip == 0x99999999);
    assert(state.gpr[0] == 60);

    /* Now all 3 blocks are compiled and backward-linked! Run from 0x1000 again in a single chained step */
    state.eip = 0x1000;
    state.gpr[0] = 0;
    state.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;

    uint64_t prev_transitions = engine.linked_transitions;
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.eip == 0x99999999);
    assert(state.gpr[0] == 60);
    assert(engine.linked_transitions - prev_transitions >= 2);

    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

/* 2. Both sides of conditional exits and later linking of the cold side */
static void test_conditional_both_sides(void)
{
    /*
     * 0x1000: test ecx, ecx; jz 0x1020; mov edx, 1; ret (fallthrough: edx = 1)
     * 0x1020: mov edx, 2; ret                            (taken: edx = 2)
     */
    uint8_t code[64];
    memset(code, 0x90, sizeof(code));
    /* 0x1000: 85 c9 (test ecx, ecx); 74 1c (jz 0x1020 -> rel offset 28); ba 01 00 00 00 (mov edx, 1); c3 (ret) */
    code[0] = 0x85; code[1] = 0xc9;
    code[2] = 0x74; code[3] = 0x1c;
    code[4] = 0xba; code[5] = 1; code[6] = 0; code[7] = 0; code[8] = 0;
    code[9] = 0xc3;
    /* 0x1020: ba 02 00 00 00 (mov edx, 2); c3 (ret) */
    code[32] = 0xba; code[33] = 2; code[34] = 0; code[35] = 0; code[36] = 0;
    code[37] = 0xc3;

    TestSource src = {0x1000, code, sizeof(code)};
    PwVmBackend vm;
    PwX86Engine engine;
    PwX86CacheEntry entries[16];
    PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
    state.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state.gpr[4] = 0x88888888;

    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_engine_init(&engine, &vm, entries, 16, 65536, 1, test_source_view, &src) == PW_OK);
    assert(pw_x86_engine_set_chaining(&engine, 1) == PW_OK);

    PwX86StepReport step;
    /* First: ecx = 1 -> fallthrough path taken (not zero) -> exits to 0x1004 */
    state.gpr[1] = 1;
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    if (state.eip == 0x1004) {
        assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    }
    assert(state.gpr[2] == 1);
    assert(state.eip == 0x88888888);

    /* Second: ecx = 0 -> taken branch (cold side) taken -> exits to 0x1020 */
    state.eip = 0x1000;
    state.gpr[1] = 0;
    state.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state.gpr[4] = 0x88888888;
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    /* 0x1020 compiled on demand, step completed */
    if (state.eip == 0x1020) {
        assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    }
    assert(state.gpr[2] == 2);
    assert(state.eip == 0x88888888);

    /* Now both sides are compiled; re-run fallthrough side to verify full chaining */
    state.eip = 0x1000;
    state.gpr[1] = 1;
    state.gpr[2] = 0;
    state.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state.gpr[4] = 0x88888888;
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.gpr[2] == 1);
    assert(state.eip == 0x88888888);

    /* Re-run taken side to verify full chaining */
    state.eip = 0x1000;
    state.gpr[1] = 0;
    state.gpr[2] = 0;
    state.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state.gpr[4] = 0x88888888;
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.gpr[2] == 2);
    assert(state.eip == 0x88888888);

    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

/* 3. Backward loop budget/safepoint enforcement */
static void test_backward_loop_safepoint(void)
{
    /*
     * 0x1000: inc eax; dec ecx; jnz 0x1000; ret
     * 40 (inc eax); 49 (dec ecx); 75 fa (jnz -6 -> 0x1000); c3 (ret)
     */
    uint8_t loop[] = {0x40, 0x49, 0x75, 0xfc, 0xc3};
    TestSource src = {0x1000, loop, sizeof(loop)};
    PwVmBackend vm;
    PwX86Engine engine;
    PwX86CacheEntry entries[16];
    PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
    state.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state.gpr[4] = 0x77777777;
    state.gpr[0] = 0;    /* eax = 0 */
    state.gpr[1] = 100;  /* ecx = 100 iterations */

    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_engine_init(&engine, &vm, entries, 16, 65536, 1, test_source_view, &src) == PW_OK);
    assert(pw_x86_engine_set_chaining(&engine, 1) == PW_OK);
    assert(pw_x86_engine_set_quantum(&engine, 32) == PW_OK); /* Quantum = 32 */

    PwX86StepReport step;
    /* Step 1: should execute 32 iterations and safepoint yield */
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.gpr[0] == 32);
    assert(state.gpr[1] == 68);
    assert(state.eip == 0x1000); /* safepoint returns to loop start */
    assert(engine.safepoint_returns == 1);

    /* Step 2: another 32 iterations */
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.gpr[0] == 64);
    assert(state.gpr[1] == 36);
    assert(state.eip == 0x1000);
    assert(engine.safepoint_returns == 2);

    /* Step 3: another 32 iterations */
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.gpr[0] == 96);
    assert(state.gpr[1] == 4);
    assert(state.eip == 0x1000);
    assert(engine.safepoint_returns == 3);

    /* Step 4: finishes the remaining 4 iterations and exits via ret */
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    if (state.eip == 0x1004) {
        assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    }
    assert(state.gpr[0] == 100);
    assert(state.gpr[1] == 0);
    assert(state.eip == 0x77777777);

    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

/* 4. Unresolved-target fallback followed by successful link */
static void test_unresolved_target_fallback(void)
{
    /* 0x1000: jmp 0x1010; 0x1010: mov eax, 42; ret */
    uint8_t code[32];
    memset(code, 0x90, sizeof(code));
    code[0] = 0xeb; code[1] = 0x0e; /* 0x1000: jmp 0x1010 */
    code[16] = 0xb8; code[17] = 42; code[18] = 0; code[19] = 0; code[20] = 0; code[21] = 0xc3;

    TestSource src = {0x1000, code, sizeof(code)};
    PwVmBackend vm;
    PwX86Engine engine;
    PwX86CacheEntry entries[16];
    PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
    state.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state.gpr[4] = 0x66666666;

    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_engine_init(&engine, &vm, entries, 16, 65536, 1, test_source_view, &src) == PW_OK);
    assert(pw_x86_engine_set_chaining(&engine, 1) == PW_OK);

    PwX86StepReport step;
    /* Step 1: 0x1000 compiles. 0x1010 not in cache. Unlinked fallback stub triggers. */
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.eip == 0x1010);
    assert(state.last_exit_slot != 0);

    /* Step 2: 0x1010 compiles, backward link resolves the pending slot */
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.gpr[0] == 42);
    assert(state.eip == 0x66666666);
    assert(engine.successful_links >= 1);

    /* Step 3: Run from 0x1000 again -> direct chain executes without unlinked fallback */
    state.eip = 0x1000;
    state.gpr[0] = 0;
    state.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state.gpr[4] = 0x66666666;
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.gpr[0] == 42);
    assert(state.eip == 0x66666666);
    assert(state.last_exit_slot == 0);

    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

/* 5. Reset/generation invalidation, including stale-target non-execution */
static void test_generation_reset_invalidation(void)
{
    uint8_t code[32];
    memset(code, 0x90, sizeof(code));
    code[0] = 0xeb; code[1] = 0x0e; /* jmp 0x1010 */
    code[16] = 0xb8; code[17] = 77; code[18] = 0; code[19] = 0; code[20] = 0; code[21] = 0xc3;

    TestSource src = {0x1000, code, sizeof(code)};
    PwVmBackend vm;
    PwX86Engine engine;
    PwX86CacheEntry entries[16];
    PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
    state.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state.gpr[4] = 0x55555555;

    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_engine_init(&engine, &vm, entries, 16, 65536, 1, test_source_view, &src) == PW_OK);
    assert(pw_x86_engine_set_chaining(&engine, 1) == PW_OK);

    PwX86StepReport step;
    /* Compile both blocks and link */
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(engine.successful_links >= 1);

    /* Reset engine with new generation 2 */
    assert(pw_x86_engine_reset(&engine, 2) == PW_OK);
    assert(engine.unlinks >= 1);

    /* Verify that entry slots were invalidated */
    for (size_t i = 0; i < 16; i++) {
        assert(!entries[i].used);
        assert(!entries[i].link_slots[0].is_linked);
    }

    /* Mutate source to prove stale code is not executed */
    code[17] = 99;
    state.eip = 0x1000;
    state.gpr[0] = 0;
    state.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state.gpr[4] = 0x55555555;
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.gpr[0] == 99); /* New code was compiled and executed! */

    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

/* 6. Link-publication failure and rollback */
static int failing_protect(void *ctx, const PwVmRegion *region, size_t off, size_t bytes, unsigned prot)
{
    /* Fail when sealing executable code */
    if (prot == (PW_PROT_READ | PW_PROT_EXEC)) return PW_ERR_VM;
    PwVmBackend vm;
    if (pw_vm_posix_backend(&vm) != PW_OK) return PW_ERR_VM;
    return vm.protect(ctx, region, off, bytes, prot);
}

static void test_publication_failure_rollback(void)
{
    uint8_t code[] = {0x90, 0xc3};
    TestSource src = {0x1000, code, sizeof(code)};
    PwVmBackend vm;
    PwX86Engine engine;
    PwX86CacheEntry entries[16];
    PwX86State state = {.eip = 0x1000};

    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_engine_init(&engine, &vm, entries, 16, 65536, 1, test_source_view, &src) == PW_OK);

    /* Substitute protect with failing protect */
    PwVmBackend failing_vm = vm;
    failing_vm.protect = failing_protect;
    engine.backend = &failing_vm;

    PwX86StepReport step;
    int status = pw_x86_engine_step(&engine, &state, &step);
    assert(status == PW_ERR_VM);
    assert(engine.failed == 1);

    /* Restore real backend to clean up */
    engine.backend = &vm;
    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

/* 7. Exact retirement/EIP/state parity against unchained execution */
static void test_retirement_parity(void)
{
    /* Complex sequence: ALU, conditional branch, arithmetic flags, loop */
    uint8_t code[] = {
        /* 0x1000: loop_start */
        0x05, 0x37, 0x13, 0x00, 0x00,       /* add eax, 0x1337 */
        0x31, 0xc2,                         /* xor edx, eax */
        0x29, 0xd8,                         /* sub eax, ebx */
        0x81, 0xe2, 0xff, 0xff, 0xff, 0x7f, /* and edx, 0x7fffffff */
        0x40,                               /* inc eax */
        0x49,                               /* dec ecx */
        0x75, 0xed,                         /* jnz 0x1000 (-19) */
        0xc3                                /* ret */
    };
    TestSource src = {0x1000, code, sizeof(code)};
    PwVmBackend vm;
    assert(pw_vm_posix_backend(&vm) == PW_OK);

    /* 1. Unchained run */
    PwX86Engine engine_unchained;
    PwX86CacheEntry entries_u[16];
    assert(pw_x86_engine_init(&engine_unchained, &vm, entries_u, 16, 65536, 1, test_source_view, &src) == PW_OK);
    assert(pw_x86_engine_set_chaining(&engine_unchained, 0) == PW_OK);

    PwX86State state_u = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
    state_u.gpr[0] = 0x12345678;
    state_u.gpr[1] = 50; /* 50 iterations */
    state_u.gpr[2] = 0xdeadbeef;
    state_u.gpr[3] = 0x3;
    state_u.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state_u.gpr[4] = 0x44444444;

    uint64_t unchained_dispatches = 0;
    while (state_u.eip >= 0x1000 && state_u.eip < 0x1000 + sizeof(code)) {
        PwX86StepReport rep;
        assert(pw_x86_engine_step(&engine_unchained, &state_u, &rep) == PW_OK);
        unchained_dispatches++;
    }

    /* 2. Chained run */
    PwX86Engine engine_chained;
    PwX86CacheEntry entries_c[16];
    assert(pw_x86_engine_init(&engine_chained, &vm, entries_c, 16, 65536, 1, test_source_view, &src) == PW_OK);
    assert(pw_x86_engine_set_chaining(&engine_chained, 1) == PW_OK);

    PwX86State state_c = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
    state_c.gpr[0] = 0x12345678;
    state_c.gpr[1] = 50;
    state_c.gpr[2] = 0xdeadbeef;
    state_c.gpr[3] = 0x3;
    state_c.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state_c.gpr[4] = 0x44444444;

    uint64_t chained_dispatches = 0;
    while (state_c.eip >= 0x1000 && state_c.eip < 0x1000 + sizeof(code)) {
        PwX86StepReport rep;
        assert(pw_x86_engine_step(&engine_chained, &state_c, &rep) == PW_OK);
        chained_dispatches++;
    }

    /* Exact parity assertion */
    for (int i = 0; i < 8; i++) {
        assert(state_u.gpr[i] == state_c.gpr[i]);
    }
    assert(state_u.eip == state_c.eip);
    assert(state_u.eflags == state_c.eflags);
    assert(engine_unchained.retired_instructions == engine_chained.retired_instructions);

    /* Material reduction in dispatcher entries */
    assert(chained_dispatches < unchained_dispatches);

    assert(pw_x86_engine_destroy(&engine_unchained) == PW_OK);
    assert(pw_x86_engine_destroy(&engine_chained) == PW_OK);
}

/* 8. Imports, indirect control flow, faults and x87 traps remaining unchained */
static void test_unchained_operations(void)
{
    PwVmBackend vm;
    assert(pw_vm_posix_backend(&vm) == PW_OK);

    /* Indirect jump: ff e0 (jmp eax) */
    uint8_t ind_jmp[] = {0xff, 0xe0};
    TestSource src1 = {0x1000, ind_jmp, sizeof(ind_jmp)};
    PwX86Engine eng1;
    PwX86CacheEntry ent1[4];
    assert(pw_x86_engine_init(&eng1, &vm, ent1, 4, 65536, 1, test_source_view, &src1) == PW_OK);
    assert(pw_x86_engine_set_chaining(&eng1, 1) == PW_OK);
    PwX86State s1 = {.eip = 0x1000};
    s1.gpr[0] = 0x1050;
    PwX86StepReport rep1;
    assert(pw_x86_engine_step(&eng1, &s1, &rep1) == PW_OK);
    assert(s1.eip == 0x1050);
    assert(eng1.linked_transitions == 0); /* Unchained */
    assert(pw_x86_engine_destroy(&eng1) == PW_OK);

    /* Memory fault: mov esp, 0; push esp */
    uint8_t fault[] = {0xbc, 0, 0, 0, 0, 0x50};
    TestSource src2 = {0x2000, fault, sizeof(fault)};
    PwX86Engine eng2;
    PwX86CacheEntry ent2[4];
    assert(pw_x86_engine_init(&eng2, &vm, ent2, 4, 65536, 1, test_source_view, &src2) == PW_OK);
    assert(pw_x86_engine_set_chaining(&eng2, 1) == PW_OK);
    PwX86State s2 = {.eip = 0x2000};
    PwX86StepReport rep2;
    assert(pw_x86_engine_step(&eng2, &s2, &rep2) == PW_ERR_VM);
    assert(s2.eip == 0x2005); /* Precise faulting PC */
    assert(eng2.linked_transitions == 0);
    assert(pw_x86_engine_destroy(&eng2) == PW_OK);

    /* x87 trap */
    uint8_t x87_trap[] = {0xd9, 0xe8, 0xd9, 0xee, 0xde, 0xf9};
    TestSource src3 = {0x3000, x87_trap, sizeof(x87_trap)};
    PwX86Engine eng3;
    PwX86CacheEntry ent3[4];
    assert(pw_x86_engine_init(&eng3, &vm, ent3, 4, 65536, 1, test_source_view, &src3) == PW_OK);
    assert(pw_x86_engine_set_chaining(&eng3, 1) == PW_OK);
    PwX86State s3 = {.eip = 0x3000};
    pw_guest_fp_init(&s3.fp);
    s3.fp.x87_control = (uint16_t)(s3.fp.x87_control & ~4u);
    PwX86StepReport rep3;
    assert(pw_x86_engine_step(&eng3, &s3, &rep3) == PW_ERR_X87_TRAP);
    assert(s3.eip == 0x3004);
    assert(eng3.linked_transitions == 0);
    assert(pw_x86_engine_destroy(&eng3) == PW_OK);
}

/* 9. Code arena exhaustion */
static void test_code_arena_exhaustion(void)
{
    /* 4096-byte arena */
    uint8_t nops[64];
    memset(nops, 0x90, sizeof(nops));
    nops[63] = 0xc3;

    TestSource src = {0x1000, nops, sizeof(nops)};
    PwVmBackend vm;
    PwX86Engine engine;
    PwX86CacheEntry entries[64];
    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_engine_init(&engine, &vm, entries, 64, 4096, 1, test_source_view, &src) == PW_OK);

    int hit_limit = 0;
    for (uint32_t pc = 0x1000; pc < 0x1000 + 64; pc++) {
        PwX86State s = {.eip = pc, .stack_low = 0x03000000, .stack_high = 0x03010000};
        s.gpr[4] = 0x0300ff00;
        *(uint32_t *)(uintptr_t)s.gpr[4] = 0x11111111;
        PwX86StepReport rep;
        int status = pw_x86_engine_step(&engine, &s, &rep);
        if (status == PW_ERR_LIMIT) {
            hit_limit = 1;
            break;
        }
        assert(status == PW_OK);
    }
    assert(hit_limit == 1);
    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

/* 10. W^X backend spy proving no RWX state and bounded transitions */
typedef struct WxSpy {
    PwVmBackend real;
    uint32_t protect_calls;
    uint32_t rw_transitions;
    uint32_t rx_transitions;
} WxSpy;

static int wx_spy_protect(void *ctx, const PwVmRegion *region, size_t off, size_t bytes, unsigned prot)
{
    WxSpy *spy = (WxSpy *)ctx;
    spy->protect_calls++;

    /* Strict W^X invariant: NEVER both WRITE and EXEC */
    assert((prot & (PW_PROT_WRITE | PW_PROT_EXEC)) != (PW_PROT_WRITE | PW_PROT_EXEC));

    if (prot & PW_PROT_WRITE) spy->rw_transitions++;
    if (prot & PW_PROT_EXEC) spy->rx_transitions++;

    return spy->real.protect(spy->real.context, region, off, bytes, prot);
}

static void test_wx_backend_spy(void)
{
    uint8_t loop[] = {0x40, 0xeb, 0xfd};
    TestSource src = {0x1000, loop, sizeof(loop)};

    WxSpy spy = {0};
    assert(pw_vm_posix_backend(&spy.real) == PW_OK);

    PwVmBackend spy_backend = spy.real;
    spy_backend.context = &spy;
    spy_backend.protect = wx_spy_protect;

    PwX86Engine engine;
    PwX86CacheEntry entries[16];
    assert(pw_x86_engine_init(&engine, &spy_backend, entries, 16, 65536, 1, test_source_view, &src) == PW_OK);
    assert(pw_x86_engine_set_chaining(&engine, 1) == PW_OK);
    assert(pw_x86_engine_set_quantum(&engine, 10) == PW_OK);

    uint32_t initial_protects = spy.protect_calls;

    PwX86State state = {.eip = 0x1000};
    PwX86StepReport rep;
    assert(pw_x86_engine_step(&engine, &state, &rep) == PW_OK);

    /* Compiling 1 block requires exactly 2 transitions: RW to copy, RX to seal */
    assert(spy.protect_calls - initial_protects == 2);
    assert(spy.rw_transitions >= 1);
    assert(spy.rx_transitions >= 1);

    /*
     * Link slot updates happen in RW memory (link_slots array), NOT via mprotect!
     * Subsequent step with chaining must perform ZERO new protect calls!
     */
    uint32_t protect_after_compile = spy.protect_calls;
    assert(pw_x86_engine_step(&engine, &state, &rep) == PW_OK);
    assert(spy.protect_calls == protect_after_compile);

    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

static const PwX86CacheEntry *entry_at(const PwX86CacheEntry *entries, size_t count, uint32_t pc)
{
    for (size_t i = 0; i < count; i++)
        if (entries[i].used && entries[i].guest_pc == pc) return &entries[i];
    return NULL;
}

/* 11. Exits waiting for different targets in one bucket: compiling a target
 * links exactly the exits waiting for it, in any order, including when the
 * target is published into the bucket's own slot. */
static void test_pending_exits_share_a_bucket(void)
{
    /* At capacity 8, 0x1060, 0x1070 and 0x11a0 all hash to bucket 0. */
    static const uint32_t sources[3] = {0x1000, 0x1010, 0x1020};
    static const uint32_t targets[3] = {0x1060, 0x1070, 0x11a0};
    uint8_t code[0x1b0];
    memset(code, 0x90, sizeof(code));
    for (unsigned i = 0; i < 3; i++) {
        uint8_t *jmp = code + (sources[i] - 0x1000);
        uint8_t *target = code + (targets[i] - 0x1000);
        int32_t rel = (int32_t)(targets[i] - (sources[i] + 5));
        jmp[0] = 0xe9; memcpy(jmp + 1, &rel, 4);             /* jmp target */
        target[0] = 0xb8; target[1] = (uint8_t)(10 * (i + 1));  /* mov eax, 10*(i+1) */
        target[2] = target[3] = target[4] = 0; target[5] = 0xc3; /* ret */
    }

    TestSource src = {0x1000, code, sizeof(code)};
    PwVmBackend vm;
    PwX86Engine engine;
    PwX86CacheEntry entries[8];
    PwX86State state = {.stack_low = 0x03000000, .stack_high = 0x03010000};
    PwX86StepReport step;

    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_engine_init(&engine, &vm, entries, 8, 65536, 1, test_source_view, &src) == PW_OK);
    assert(pw_x86_engine_set_chaining(&engine, 1) == PW_OK);

    /* Compile the three jumps; each exit waits, unlinked. */
    for (unsigned i = 0; i < 3; i++) {
        state.eip = sources[i];
        assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
        assert(state.eip == targets[i]);
    }
    assert(engine.successful_links == 0 && entries[0].pending_head != 0);

    /* Compile the targets out of order: each links only its own waiter. */
    static const unsigned order[3] = {1, 0, 2};
    for (unsigned n = 0; n < 3; n++) {
        unsigned i = order[n];
        state.eip = targets[i];
        state.gpr[4] = 0x0300ff00;
        *(uint32_t *)(uintptr_t)state.gpr[4] = 0x44444444;
        assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
        assert(state.eip == 0x44444444 && state.gpr[0] == 10 * (i + 1));
        assert(engine.successful_links == n + 1);
        for (unsigned j = 0; j < 3; j++) {
            const PwX86CacheEntry *jump = entry_at(entries, 8, sources[j]);
            unsigned compiled = j == order[0] || (n >= 1 && j == order[1]) || n == 2;
            assert(jump && jump->link_slots[0].is_linked == compiled);
        }
    }
    /* Nothing is left waiting: 0x1070, compiled first, took slot 0 and kept
     * the bucket's chain for the other two. A jump now runs through to its
     * target in one step. */
    assert(entries[0].used && entries[0].pending_head == 0);
    assert(entry_at(entries, 8, 0x1070) == &entries[0]);
    for (unsigned i = 0; i < 3; i++) {
        state.eip = sources[i];
        state.gpr[0] = 0;
        state.gpr[4] = 0x0300ff00;
        *(uint32_t *)(uintptr_t)state.gpr[4] = 0x44444444;
        uint64_t transitions = engine.linked_transitions;
        assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
        assert(state.eip == 0x44444444 && state.gpr[0] == 10 * (i + 1));
        assert(engine.linked_transitions == transitions + 1);
    }

    /* A reset forgets every waiter. */
    assert(pw_x86_engine_reset(&engine, 2) == PW_OK);
    for (unsigned i = 0; i < 8; i++)
        assert(!entries[i].pending_head && !entries[i].pending_next[0] && !entries[i].pending_next[1]);
    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

/* 12. A direct call chains to its target once the return address is pushed */
static void test_direct_call_chains(void)
{
    /*
     * 0x1000: mov eax, 5; call 0x1020; add eax, 1; ret   (return lands at 0x100a)
     * 0x1020: add eax, 100; ret
     */
    uint8_t code[64];
    memset(code, 0x90, sizeof(code));
    code[0] = 0xb8; code[1] = 5; code[2] = 0; code[3] = 0; code[4] = 0;
    code[5] = 0xe8; code[6] = 0x16; code[7] = 0; code[8] = 0; code[9] = 0; /* 0x100a + 0x16 */
    code[10] = 0x83; code[11] = 0xc0; code[12] = 1;
    code[13] = 0xc3;
    code[32] = 0x83; code[33] = 0xc0; code[34] = 100;
    code[35] = 0xc3;

    TestSource src = {0x1000, code, sizeof(code)};
    PwVmBackend vm;
    PwX86Engine engine;
    PwX86CacheEntry entries[16];
    PwX86StepReport step;
    PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
    const uint32_t esp = 0x0300ff00;

    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_engine_init(&engine, &vm, entries, 16, 65536, 1, test_source_view, &src) == PW_OK);
    assert(pw_x86_engine_set_chaining(&engine, 1) == PW_OK);
    for (unsigned pass = 0; pass < 2; pass++) {
        uint64_t transitions = engine.linked_transitions;
        state.eip = 0x1000; state.gpr[0] = 0; state.gpr[4] = esp;
        *(uint32_t *)(uintptr_t)esp = 0x99999999;
        /* The call's block: on the second pass it runs straight into 0x1020. */
        assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
        if (!pass) {
            assert(state.eip == 0x1020 && state.gpr[0] == 5);
            assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
        } else {
            assert(engine.linked_transitions - transitions == 1);
        }
        /* 0x1020 returned to the pushed address, below the caller's slot. */
        assert(state.eip == 0x100a && state.gpr[0] == 105 && state.gpr[4] == esp);
        assert(*(uint32_t *)(uintptr_t)(esp - 4) == 0x100a);
        assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
        assert(state.eip == 0x99999999 && state.gpr[0] == 106 && state.gpr[4] == esp + 4);
    }
    /* A call whose push faults stops at the call with nothing chained. */
    state.eip = 0x1000; state.gpr[4] = 0x03000002;
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_ERR_VM);
    assert(state.eip == 0x1005 && state.gpr[4] == 0x03000002);
    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

/* ---- indirect targets: rets and indirect calls/jumps without the dispatcher ---- */

static void indirect_engine(PwX86Engine *engine, PwVmBackend *vm, PwX86CacheEntry *entries,
                            uint32_t capacity, TestSource *src, unsigned residency, unsigned lazy)
{
    assert(pw_vm_posix_backend(vm) == PW_OK);
    assert(pw_x86_engine_init(engine, vm, entries, capacity, 1u << 20, 1, test_source_view, src) == PW_OK);
    assert(pw_x86_engine_set_chaining(engine, 1) == PW_OK);
    assert(pw_x86_engine_set_residency(engine, residency) == PW_OK);
    assert(pw_x86_engine_set_lazy_flags(engine, lazy) == PW_OK);
    assert(pw_x86_engine_set_indirect(engine, 1) == PW_OK);
}

/* Run from pc until EIP reaches stop; returns the number of steps. */
static unsigned run_until(PwX86Engine *engine, PwX86State *state, uint32_t pc, uint32_t stop)
{
    PwX86StepReport step;
    unsigned steps = 0;

    state->eip = pc;
    while (state->eip != stop) {
        assert(pw_x86_engine_step(engine, state, &step) == PW_OK);
        assert(++steps < 100000);
    }
    return steps;
}

/* 13. A ret enters its caller's continuation straight from the table, in every
 * engine mode; a ret whose target was never dispatched still returns. */
static void test_indirect_ret(void)
{
    /*
     * 0x1000: mov eax, 5; call 0x1020   (the call is at 0x1005)
     * 0x100a: add eax, 1; ret
     * 0x1020: add eax, 100; ret
     */
    uint8_t code[64];
    memset(code, 0x90, sizeof(code));
    code[0] = 0xb8; code[1] = 5; code[2] = 0; code[3] = 0; code[4] = 0;
    code[5] = 0xe8; code[6] = 0x16; code[7] = 0; code[8] = 0; code[9] = 0;
    code[10] = 0x83; code[11] = 0xc0; code[12] = 1; code[13] = 0xc3;
    code[32] = 0x83; code[33] = 0xc0; code[34] = 100; code[35] = 0xc3;
    const uint32_t esp = 0x0300ff00;

    for (unsigned mode = 0; mode < 4; mode++) {
        TestSource src = {0x1000, code, sizeof(code)};
        PwVmBackend vm;
        PwX86Engine engine;
        PwX86CacheEntry entries[16];
        PwX86State state = {.stack_low = 0x03000000, .stack_high = 0x03010000};

        indirect_engine(&engine, &vm, entries, 16, &src, mode & 1, mode >> 1);
        for (unsigned pass = 0; pass < 2; pass++) {
            uint64_t transitions = engine.linked_transitions;
            state.gpr[0] = 0; state.gpr[4] = esp;
            *(uint32_t *)(uintptr_t)esp = 0x99999999;
            unsigned steps = run_until(&engine, &state, 0x1000, 0x99999999);
            assert(state.gpr[0] == 106 && state.gpr[4] == esp + 4);
            /* Warm: the call chains and the callee's ret enters 0x100a from
             * the table, all in one step; only the outer ret returns. */
            if (pass) assert(steps == 1 && engine.linked_transitions - transitions == 2);
            else assert(steps == 3);
        }
        assert(pw_x86_engine_destroy(&engine) == PW_OK);
    }
}

/* 14. An indirect jump whose target changes: each target is entered from the
 * table once dispatched, a target sharing a slot with another is refused and
 * taken by the dispatcher, and the slot then names the newer one. */
static void test_indirect_target_changes(void)
{
    /*
     * 0x1000: jmp eax
     * 0x1100: mov edx, 1; ret
     * 0x1200: mov edx, 2; ret
     * 0x3102: mov edx, 3; ret      (the same slot as 0x1100)
     */
    static uint8_t code[0x2200];
    const uint32_t targets[3] = {0x1100, 0x1200, 0x3102};
    const uint32_t mask = PW_X86_ENGINE_INDIRECT_SLOTS - 1;
    const unsigned order[] = {0, 1, 0, 1, 2, 0, 2, 2, 1, 0};

    memset(code, 0x90, sizeof(code));
    code[0] = 0xff; code[1] = 0xe0;
    for (unsigned t = 0; t < 3; t++) {
        uint8_t *p = code + (targets[t] - 0x1000);
        p[0] = 0xba; p[1] = (uint8_t)(t + 1); p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0xc3;
    }
    assert(pw_x86_indirect_slot(0x1100, mask) == pw_x86_indirect_slot(0x3102, mask));
    assert(pw_x86_indirect_slot(0x1100, mask) != pw_x86_indirect_slot(0x1200, mask));

    TestSource src = {0x1000, code, sizeof(code)};
    PwVmBackend vm;
    PwX86Engine engine;
    PwX86CacheEntry entries[16];
    PwX86State state = {.stack_low = 0x03000000, .stack_high = 0x03010000};
    indirect_engine(&engine, &vm, entries, 16, &src, 1, 1);
    for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        const unsigned t = order[i];
        const PwX86IndirectTarget *slot = &engine.indirect_targets[pw_x86_indirect_slot(targets[t], mask)];
        const unsigned warm = slot->guest_pc == targets[t] && slot->host_code;

        state.gpr[0] = targets[t]; state.gpr[2] = 0; state.gpr[4] = 0x0300ff00;
        *(uint32_t *)(uintptr_t)state.gpr[4] = 0x88888888;
        unsigned steps = run_until(&engine, &state, 0x1000, 0x88888888);
        assert(state.gpr[2] == t + 1);
        /* A warm target takes one step (jmp eax straight into it; its ret
         * misses); a cold or displaced one goes through the dispatcher. */
        assert(steps == (warm ? 1u : 2u));
        assert(slot->guest_pc == targets[t]);
    }
    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

/* 15. A reset discards every target with the code it pointed into: after the
 * guest rewrites a target, the next run executes the new code. */
static void test_indirect_reset_after_code_change(void)
{
    /* 0x1000: jmp eax; 0x1010: mov edx, 7; ret */
    uint8_t code[32];
    memset(code, 0x90, sizeof(code));
    code[0] = 0xff; code[1] = 0xe0;
    code[16] = 0xba; code[17] = 7; code[18] = 0; code[19] = 0; code[20] = 0; code[21] = 0xc3;
    TestSource src = {0x1000, code, sizeof(code)};
    PwVmBackend vm;
    PwX86Engine engine;
    PwX86CacheEntry entries[16];
    PwX86State state = {.stack_low = 0x03000000, .stack_high = 0x03010000};
    indirect_engine(&engine, &vm, entries, 16, &src, 1, 1);

    for (unsigned round = 0; round < 3; round++) {
        state.gpr[0] = 0x1010; state.gpr[4] = 0x0300ff00;
        *(uint32_t *)(uintptr_t)state.gpr[4] = 0x88888888;
        unsigned steps = run_until(&engine, &state, 0x1000, 0x88888888);
        assert(state.gpr[2] == (round < 2 ? 7u : 9u));
        assert(steps == (round == 1 ? 1u : 2u));
        if (round == 1) {
            code[17] = 9;                       /* the guest rewrites 0x1010 */
            assert(pw_x86_engine_reset(&engine, 2) == PW_OK);
            for (uint32_t i = 0; i < PW_X86_ENGINE_INDIRECT_SLOTS; i++)
                assert(!engine.indirect_targets[i].host_code);
        }
    }
    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

/* 16. Recursion 2000 calls deep: every ret lands correctly, and they enter
 * their continuation without the dispatcher. */
static void test_indirect_deep_recursion(void)
{
    /*
     * 0x1000: inc eax; dec ecx; jz 0x1009; call 0x1000
     * 0x1009: ret
     */
    uint8_t code[16] = {0x40, 0x49, 0x74, 0x05, 0xe8, 0xf7, 0xff, 0xff, 0xff, 0xc3};
    TestSource src = {0x1000, code, sizeof(code)};
    PwVmBackend vm;
    PwX86Engine engine;
    PwX86CacheEntry entries[16];
    PwX86State state = {.stack_low = 0x03000000, .stack_high = 0x03010000};
    const uint32_t esp = 0x0300ff00, depth = 2000;

    indirect_engine(&engine, &vm, entries, 16, &src, 1, 1);
    for (unsigned pass = 0; pass < 2; pass++) {
        state.gpr[0] = 0; state.gpr[1] = depth; state.gpr[4] = esp;
        *(uint32_t *)(uintptr_t)esp = 0x66666666;
        unsigned steps = run_until(&engine, &state, 0x1000, 0x66666666);
        assert(state.gpr[0] == depth && state.gpr[1] == 0 && state.gpr[4] == esp + 4);
        /* About 6000 transitions: one step per quantum, plus the cold ones. */
        assert(steps <= 3 * depth / PW_X86_ENGINE_DEFAULT_QUANTUM + 6);
    }
    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

/* 17. A jump to itself cannot run away: the lookup spends the chain budget
 * and the dispatcher regains control at every safepoint. */
static void test_indirect_budget(void)
{
    uint8_t code[4] = {0xff, 0xe0, 0x90, 0x90};     /* 0x1000: jmp eax */
    TestSource src = {0x1000, code, sizeof(code)};
    PwVmBackend vm;
    PwX86Engine engine;
    PwX86CacheEntry entries[16];
    PwX86StepReport step;
    PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};

    indirect_engine(&engine, &vm, entries, 16, &src, 1, 1);
    assert(pw_x86_engine_set_quantum(&engine, 16) == PW_OK);
    state.gpr[0] = 0x1000;
    for (unsigned i = 0; i < 4; i++) {
        uint64_t transitions = engine.linked_transitions;
        assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK && state.eip == 0x1000);
        assert(engine.linked_transitions - transitions == 15 && engine.safepoint_returns == i + 1);
    }
    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

/* 18. Off by default, and inert without chaining: every exit returns. */
static void test_indirect_off(void)
{
    /* 0x1000: jmp eax; 0x1004: mov edx, 1; ret */
    uint8_t code[10] = {0xff, 0xe0, 0x90, 0x90, 0xba, 1, 0, 0, 0, 0xc3};

    for (unsigned variant = 0; variant < 2; variant++) {
        TestSource src = {0x1000, code, sizeof(code)};
        PwVmBackend vm;
        PwX86Engine engine;
        PwX86CacheEntry entries[16];
        PwX86State state = {.stack_low = 0x03000000, .stack_high = 0x03010000};

        assert(pw_vm_posix_backend(&vm) == PW_OK);
        assert(pw_x86_engine_init(&engine, &vm, entries, 16, 65536, 1, test_source_view, &src) == PW_OK);
        assert(!engine.indirect_enabled && !engine.indirect_targets);
        if (variant) assert(pw_x86_engine_set_indirect(&engine, 1) == PW_OK);  /* no chaining */
        else assert(pw_x86_engine_set_chaining(&engine, 1) == PW_OK);         /* no table */
        for (unsigned pass = 0; pass < 3; pass++) {
            state.gpr[0] = 0x1004; state.gpr[2] = 0; state.gpr[4] = 0x0300ff00;
            *(uint32_t *)(uintptr_t)state.gpr[4] = 0x88888888;
            assert(run_until(&engine, &state, 0x1000, 0x88888888) == 2 && state.gpr[2] == 1);
        }
        /* Turning the table off keeps it for the blocks translated with it. */
        if (variant) {
            assert(pw_x86_engine_set_indirect(&engine, 0) == PW_OK && engine.indirect_targets);
            assert(!engine.indirect_enabled);
        }
        assert(pw_x86_engine_destroy(&engine) == PW_OK);
    }
    assert(pw_x86_engine_set_indirect(NULL, 1) == PW_ERR_PRECONDITION);
}

/* Counters off: the same guest results and safepoints, and no statistics. */
static void test_counters_off(void)
{
    uint8_t loop[] = {0x40, 0x49, 0x75, 0xfc, 0xc3};  /* inc eax; dec ecx; jnz; ret */
    TestSource src = {0x1000, loop, sizeof(loop)};
    uint32_t retired[2] = {0, 0};
    uint64_t transitions[2] = {0, 0};

    for (unsigned counters = 0; counters < 2; counters++) {
        PwVmBackend vm;
        PwX86Engine engine;
        PwX86CacheEntry entries[16];
        PwX86StepReport step;
        PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
        unsigned steps = 0;

        state.gpr[4] = 0x0300ff00;
        *(uint32_t *)(uintptr_t)state.gpr[4] = 0x77777777;
        state.gpr[1] = 100;
        assert(pw_vm_posix_backend(&vm) == PW_OK);
        assert(pw_x86_engine_init(&engine, &vm, entries, 16, 65536, 1, test_source_view, &src) == PW_OK);
        assert(pw_x86_engine_set_chaining(&engine, 1) == PW_OK);
        assert(pw_x86_engine_set_quantum(&engine, 32) == PW_OK);
        assert(pw_x86_engine_set_counters(&engine, counters) == PW_OK);
        while (state.eip != 0x77777777) {
            assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK && ++steps < 20);
            retired[counters] += step.retired;
        }
        assert(state.gpr[0] == 100 && state.gpr[1] == 0 && state.gpr[4] == 0x0300ff04);
        assert(engine.safepoint_returns == 3);
        transitions[counters] = engine.linked_transitions;
        assert(pw_x86_engine_destroy(&engine) == PW_OK);
    }
    assert(retired[1] == 301 && transitions[1] > 0);
    assert(retired[0] == 0 && transitions[0] == 0);
    assert(pw_x86_engine_set_counters(NULL, 0) == PW_ERR_PRECONDITION);
}

int main(void)
{
    PwVmBackend vm;
    PwVmRegion stack_region;
    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(vm.reserve_at(vm.context, 0x03000000, 65536, vm.page_bytes, &stack_region) == PW_OK);
    assert(vm.commit(vm.context, &stack_region, 0, stack_region.bytes, PW_PROT_READ | PW_PROT_WRITE) == PW_OK);

    test_unconditional_chains();
    test_conditional_both_sides();
    test_backward_loop_safepoint();
    test_unresolved_target_fallback();
    test_generation_reset_invalidation();
    test_publication_failure_rollback();
    test_retirement_parity();
    test_unchained_operations();
    test_code_arena_exhaustion();
    test_wx_backend_spy();
    test_pending_exits_share_a_bucket();
    test_direct_call_chains();
    test_indirect_ret();
    test_indirect_target_changes();
    test_indirect_reset_after_code_change();
    test_indirect_deep_recursion();
    test_indirect_budget();
    test_indirect_off();
    test_counters_off();

    assert(vm.release(vm.context, &stack_region) == PW_OK);
    printf("all 19 chaining, indirect target and counter tests passed successfully\n");
    return 0;
}
