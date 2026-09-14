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

/* 1. Load-once/use-many and write-many/store-once sequences */
static void test_load_once_write_many(void)
{
    /*
     * 0x1000:
     *   mov eax, 10
     *   add eax, 5
     *   add eax, 15
     *   add eax, 20
     *   ret
     */
    uint8_t code[] = {
        0xb8, 10, 0, 0, 0,       /* mov eax, 10 */
        0x83, 0xc0, 5,           /* add eax, 5 */
        0x83, 0xc0, 15,          /* add eax, 15 */
        0x83, 0xc0, 20,          /* add eax, 20 */
        0xc3                     /* ret */
    };
    TestSource src = {0x1000, code, sizeof(code)};
    PwVmBackend vm;
    PwX86Engine engine;
    PwX86CacheEntry entries[16];
    PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
    state.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;

    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_engine_init(&engine, &vm, entries, 16, 65536, 1, test_source_view, &src) == PW_OK);

    PwX86StepReport step;
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.gpr[0] == 50);
    assert(state.eip == 0x99999999);
    assert(engine.reg_stores >= 1);

    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

/* 2. All eight guest GPRs and pressure requiring spills */
static void test_all_eight_gprs_and_pressure(void)
{
    /*
     * 0x1000:
     *   mov eax, 1
     *   mov ecx, 2
     *   mov edx, 3
     *   mov ebx, 4
     *   mov ebp, 5
     *   mov esi, 6
     *   mov edi, 7
     *   add eax, 10
     *   add eax, 20  (eax is hottest)
     *   add ecx, 10  (ecx is hot)
     *   add edx, 10  (edx is hot)
     *   ret
     */
    uint8_t code[] = {
        0xb8, 1, 0, 0, 0,        /* mov eax, 1 */
        0xb9, 2, 0, 0, 0,        /* mov ecx, 2 */
        0xba, 3, 0, 0, 0,        /* mov edx, 3 */
        0xbb, 4, 0, 0, 0,        /* mov ebx, 4 */
        0xbd, 5, 0, 0, 0,        /* mov ebp, 5 */
        0xbe, 6, 0, 0, 0,        /* mov esi, 6 */
        0xbf, 7, 0, 0, 0,        /* mov edi, 7 */
        0x83, 0xc0, 10,          /* add eax, 10 */
        0x83, 0xc0, 20,          /* add eax, 20 */
        0x83, 0xc1, 10,          /* add ecx, 10 */
        0x83, 0xc2, 10,          /* add edx, 10 */
        0xc3                     /* ret */
    };
    TestSource src = {0x1000, code, sizeof(code)};
    PwVmBackend vm;
    PwX86Engine engine;
    PwX86CacheEntry entries[16];
    PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
    state.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;

    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_engine_init(&engine, &vm, entries, 16, 65536, 1, test_source_view, &src) == PW_OK);

    PwX86StepReport step;
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.gpr[0] == 31);
    assert(state.gpr[1] == 12);
    assert(state.gpr[2] == 13);
    assert(state.gpr[3] == 4);
    assert(state.gpr[5] == 5);
    assert(state.gpr[6] == 6);
    assert(state.gpr[7] == 7);
    assert(state.eip == 0x99999999);

    /* Verify contract capped at MAX_HOST_REGS = 3 */
    const PwX86CacheEntry *entry = NULL;
    assert(pw_x86_cache_lookup(&engine.cache, 0x1000, &entry) == PW_OK);
    unsigned count = 0;
    for (unsigned i = 0; i < 8; i++) {
        if (entry->entry_contract.resident_mask & (1 << i)) count++;
    }
    assert(count <= 3);

    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

/* 3. Partial-register and high-byte aliasing */
static void test_partial_register_aliasing(void)
{
    /*
     * 0x1000:
     *   mov eax, 0x11223344
     *   mov al, 0x77         (eax becomes 0x11223377)
     *   mov ah, 0x88         (eax becomes 0x11228877)
     *   mov ebx, 0x55667788
     *   mov bx, 0x1234       (ebx becomes 0x55661234)
     *   movzx ecx, al        (ecx becomes 0x00000077)
     *   movsx edx, ah        (edx becomes 0xffffff88)
     *   ret
     */
    uint8_t code[] = {
        0xb8, 0x44, 0x33, 0x22, 0x11,   /* mov eax, 0x11223344 */
        0xb0, 0x77,                     /* mov al, 0x77 */
        0xb4, 0x88,                     /* mov ah, 0x88 */
        0xbb, 0x88, 0x77, 0x66, 0x55,         /* mov ebx, 0x55667788 */
        0x66, 0xc7, 0xc3, 0x34, 0x12,         /* mov bx, 0x1234 */
        0x0f, 0xb6, 0xc8,                     /* movzx ecx, al */
        0x0f, 0xbe, 0xd4,                     /* movsx edx, ah */
        0xc3                                  /* ret */
    };
    TestSource src = {0x1000, code, sizeof(code)};
    PwVmBackend vm;
    PwX86Engine engine;
    PwX86CacheEntry entries[16];
    PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
    state.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;

    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_engine_init(&engine, &vm, entries, 16, 65536, 1, test_source_view, &src) == PW_OK);

    PwX86StepReport step;
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.gpr[0] == 0x11228877);
    assert(state.gpr[3] == 0x55661234);
    assert(state.gpr[1] == 0x00000077);
    assert(state.gpr[2] == 0xffffff88);

    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

/* 4. Byte ALU operands must observe dirty resident dwords, including AH..BH. */
static void test_dirty_resident_byte_alu_aliasing(void)
{
    /*
     *   mov eax, 0x11223344
     *   add ah, al             (eax = 0x11227744)
     *   mov ecx, 0x75
     *   mov edx, 0x12345664
     *   cmp dl, cl             (ZF = 0)
     *   ret
     *
     * Each byte consumer follows a full-dword write in the same translated
     * block.  Canonical state is therefore deliberately stale while residency
     * is enabled; reading it instead of the resident host register changes
     * both the high-byte result and the branch flags.
     */
    const uint8_t code[] = {
        0xb8, 0x44, 0x33, 0x22, 0x11,
        0x00, 0xc4,
        0xb9, 0x75, 0x00, 0x00, 0x00,
        0xba, 0x64, 0x56, 0x34, 0x12,
        0x38, 0xca,
        0xc3
    };
    TestSource src = {0x1000, (uint8_t *)code, sizeof(code)};
    PwVmBackend vm;
    PwX86Engine resident, canonical;
    PwX86CacheEntry resident_entries[16], canonical_entries[16];
    PwX86State resident_state = {
        .eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000
    };
    PwX86State canonical_state = resident_state;

    resident_state.gpr[4] = canonical_state.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)0x0300ff00 = 0x99999999;
    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_engine_init(&resident, &vm, resident_entries, 16, 65536, 1,
                              test_source_view, &src) == PW_OK);
    assert(pw_x86_engine_init(&canonical, &vm, canonical_entries, 16, 65536, 1,
                              test_source_view, &src) == PW_OK);
    assert(pw_x86_engine_set_residency(&resident, 1) == PW_OK);
    assert(pw_x86_engine_set_residency(&canonical, 0) == PW_OK);

    PwX86StepReport resident_step, canonical_step;
    assert(pw_x86_engine_step(&resident, &resident_state, &resident_step) == PW_OK);
    *(uint32_t *)(uintptr_t)0x0300ff00 = 0x99999999;
    assert(pw_x86_engine_step(&canonical, &canonical_state, &canonical_step) == PW_OK);
    assert(resident_state.gpr[0] == 0x11227744);
    assert((resident_state.eflags & 0x40u) == 0u);
    assert(memcmp(resident_state.gpr, canonical_state.gpr,
                  sizeof(resident_state.gpr)) == 0);
    assert((resident_state.eflags & 0x8d5u) ==
           (canonical_state.eflags & 0x8d5u));
    assert(resident_step.retired == canonical_step.retired);

    assert(pw_x86_engine_destroy(&resident) == PW_OK);
    assert(pw_x86_engine_destroy(&canonical) == PW_OK);
}

/* 5. ESP as operand, address base/index, and destination */
static void test_esp_operand_base_index_dest(void)
{
    /*
     * 0x1000:
     *   push 0x12345678
     *   push 0x87654321
     *   mov eax, [esp]       (eax = 0x87654321)
     *   mov ecx, [esp + 4]   (ecx = 0x12345678)
     *   pop edx              (edx = 0x87654321)
     *   pop ebx              (ebx = 0x12345678)
     *   ret
     */
    uint8_t code[] = {
        0x68, 0x78, 0x56, 0x34, 0x12,   /* push 0x12345678 */
        0x68, 0x21, 0x43, 0x65, 0x87,   /* push 0x87654321 */
        0x8b, 0x04, 0x24,               /* mov eax, [esp] */
        0x8b, 0x4c, 0x24, 0x04,         /* mov ecx, [esp + 4] */
        0x5a,                           /* pop edx */
        0x5b,                           /* pop ebx */
        0xc3                            /* ret */
    };
    TestSource src = {0x1000, code, sizeof(code)};
    PwVmBackend vm;
    PwX86Engine engine;
    PwX86CacheEntry entries[16];
    PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
    state.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;

    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_engine_init(&engine, &vm, entries, 16, 65536, 1, test_source_view, &src) == PW_OK);

    PwX86StepReport step;
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.gpr[0] == 0x87654321);
    assert(state.gpr[1] == 0x12345678);
    assert(state.gpr[2] == 0x87654321);
    assert(state.gpr[3] == 0x12345678);
    assert(state.gpr[4] == 0x0300ff04); /* popped return address */

    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

/* 6. Helper/import/fault/trap exits observing latest values */
static void test_fault_observes_latest_values(void)
{
    /*
     * 0x1000:
     *   mov eax, 0x42
     *   mov ebx, 0x84
     *   mov edx, [0]     (faults: invalid memory address 0)
     *   ret
     */
    uint8_t code[] = {
        0xb8, 0x42, 0, 0, 0,            /* mov eax, 0x42 */
        0xbb, 0x84, 0, 0, 0,            /* mov ebx, 0x84 */
        0x8b, 0x15, 0, 0, 0, 0,         /* mov edx, [0] */
        0xc3
    };
    TestSource src = {0x1000, code, sizeof(code)};
    PwVmBackend vm;
    PwX86Engine engine;
    PwX86CacheEntry entries[16];
    PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
    state.gpr[4] = 0x0300ff00;

    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_engine_init(&engine, &vm, entries, 16, 65536, 1, test_source_view, &src) == PW_OK);

    PwX86StepReport step;
    int status = pw_x86_engine_step(&engine, &state, &step);
    assert(status == PW_ERR_VM);
    /* Pre-fault barrier guarantees eax and ebx are flushed to state->gpr before fault */
    assert(state.gpr[0] == 0x42);
    assert(state.gpr[3] == 0x84);
    assert(state.eip == 0x100a); /* pointing to faulting instruction */

    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

/* 7. Different successor entry contracts forcing reconciliation spill/move */
static void test_reconciliation_mismatched_contracts(void)
{
    /*
     * Block A at 0x1000:
     *   mov eax, 10
     *   add eax, 20
     *   add eax, 30      (eax is hot)
     *   jmp 0x1020
     *
     * Block B at 0x1020:
     *   mov edx, 100
     *   add edx, 200
     *   add edx, 300     (edx is hot, eax is not used)
     *   ret
     */
    uint8_t code[64];
    memset(code, 0x90, sizeof(code));
    /* Block A at 0x1000 */
    code[0] = 0xb8; code[1] = 10; code[2] = 0; code[3] = 0; code[4] = 0;
    code[5] = 0x83; code[6] = 0xc0; code[7] = 20;
    code[8] = 0x83; code[9] = 0xc0; code[10] = 30;
    code[11] = 0xe9; code[12] = 0x10; code[13] = 0; code[14] = 0; code[15] = 0; /* jmp 0x1020 (rel 16) */
    /* Block B at 0x1020 */
    code[32] = 0xba; code[33] = 100; code[34] = 0; code[35] = 0; code[36] = 0;
    code[37] = 0x81; code[38] = 0xc2; code[39] = 200; code[40] = 0; code[41] = 0; code[42] = 0;
    code[43] = 0x81; code[44] = 0xc2; code[45] = 44; code[46] = 1; code[47] = 0; code[48] = 0; /* add edx, 300 */
    code[49] = 0xc3;

    TestSource src = {0x1000, code, sizeof(code)};
    PwVmBackend vm;
    PwX86Engine engine;
    PwX86CacheEntry entries[16];
    PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
    state.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;

    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_engine_init(&engine, &vm, entries, 16, 65536, 1, test_source_view, &src) == PW_OK);
    assert(pw_x86_engine_set_chaining(&engine, 1) == PW_OK);

    /* Compile both blocks */
    PwX86StepReport step;
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.eip == 0x1020);
    assert(state.gpr[0] == 60);

    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.eip == 0x99999999);
    assert(state.gpr[2] == 600);

    /* Now run together chained: reconciliation stub must reconcile contracts */
    state.eip = 0x1000;
    state.gpr[0] = 0;
    state.gpr[2] = 0;
    state.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;

    uint64_t prev_reconciles = engine.reg_reconciliations;
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.eip == 0x99999999);
    assert(state.gpr[0] == 60);
    assert(state.gpr[2] == 600);
    assert(engine.reg_reconciliations - prev_reconciles >= 1);

    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

/* 8. Chain invalidation and fallback to canonical memory state */
static void test_chain_invalidation_canonical_fallback(void)
{
    /*
     * 0x1000: mov eax, 10; jmp 0x1010
     * 0x1010: add eax, 20; ret
     */
    uint8_t code[32];
    memset(code, 0x90, sizeof(code));
    code[0] = 0xb8; code[1] = 10; code[2] = 0; code[3] = 0; code[4] = 0;
    code[5] = 0xeb; code[6] = 9; /* jmp 0x1010 */
    code[16] = 0x83; code[17] = 0xc0; code[18] = 20;
    code[19] = 0xc3;

    TestSource src = {0x1000, code, sizeof(code)};
    PwVmBackend vm;
    PwX86Engine engine;
    PwX86CacheEntry entries[16];
    PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
    state.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;

    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_engine_init(&engine, &vm, entries, 16, 65536, 1, test_source_view, &src) == PW_OK);
    assert(pw_x86_engine_set_chaining(&engine, 1) == PW_OK);

    PwX86StepReport step;
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.gpr[0] == 30);

    /* Reset engine (generation 2) invalidates cache and link slots */
    assert(pw_x86_engine_reset(&engine, 2) == PW_OK);
    assert(engine.unlinks >= 1);

    /* Rerun from 0x1000: falls back cleanly to compiling and canonical state */
    state.eip = 0x1000;
    state.gpr[0] = 0;
    state.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.eip == 0x1010);
    assert(state.gpr[0] == 10);

    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

/* 9. Deterministic allocation/output under identical input */
static void test_deterministic_allocation_output(void)
{
    uint8_t code[] = {
        0xb8, 1, 2, 3, 4,
        0x83, 0xc0, 10,
        0x89, 0xc3,
        0xc3
    };
    /* Lazy-flag capture is deliberately explicit and can make even a small
     * block exceed the old 256-byte deterministic-output fixture. */
    uint8_t out1[1024], out2[1024];
    PwX86Block b1 = {0}, b2 = {0};

    assert(pw_x86_translate_ext(code, sizeof(code), 0x1000, out1, sizeof(out1), &b1, 1, 1) == PW_OK);
    assert(pw_x86_translate_ext(code, sizeof(code), 0x1000, out2, sizeof(out2), &b2, 1, 1) == PW_OK);

    assert(b1.code_bytes == b2.code_bytes);
    assert(b1.canonical_entry_offset == b2.canonical_entry_offset);
    assert(b1.chain_entry_offset == b2.chain_entry_offset);
    assert(memcmp(&b1.entry_contract, &b2.entry_contract, sizeof(PwX86RegContract)) == 0);
    assert(memcmp(&b1.exit_contract, &b2.exit_contract, sizeof(PwX86RegContract)) == 0);
    assert(memcmp(out1, out2, b1.code_bytes) == 0);
}

/* 10. Parity with residency disabled through test/runtime switch */
static void test_parity_residency_switch(void)
{
    /*
     * Complex block with mixed arithmetic, branches, and register manipulation
     */
    uint8_t code[] = {
        0xb8, 10, 0, 0, 0,       /* mov eax, 10 */
        0xbb, 20, 0, 0, 0,       /* mov ebx, 20 */
        0x01, 0xd8,             /* add eax, ebx */
        0x29, 0xd8,             /* sub eax, ebx */
        0x31, 0xc9,             /* xor ecx, ecx */
        0x83, 0xc1, 5,          /* add ecx, 5 */
        0x0f, 0xaf, 0xc1,       /* imul eax, ecx */
        0xc3                    /* ret */
    };
    TestSource src = {0x1000, code, sizeof(code)};
    PwVmBackend vm;
    PwX86Engine engine_res, engine_no_res;
    PwX86CacheEntry entries1[16], entries2[16];
    PwX86State state1 = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
    PwX86State state2 = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
    state1.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state1.gpr[4] = 0x99999999;
    state2.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state2.gpr[4] = 0x99999999;

    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_engine_init(&engine_res, &vm, entries1, 16, 65536, 1, test_source_view, &src) == PW_OK);
    assert(pw_x86_engine_init(&engine_no_res, &vm, entries2, 16, 65536, 1, test_source_view, &src) == PW_OK);
    assert(pw_x86_engine_set_residency(&engine_res, 1) == PW_OK);
    assert(pw_x86_engine_set_residency(&engine_no_res, 0) == PW_OK);

    PwX86StepReport step1, step2;
    assert(pw_x86_engine_step(&engine_res, &state1, &step1) == PW_OK);
    assert(pw_x86_engine_step(&engine_no_res, &state2, &step2) == PW_OK);

    assert(state1.eip == state2.eip);
    for (unsigned i = 0; i < 8; i++) {
        assert(state1.gpr[i] == state2.gpr[i]);
    }
    assert((state1.eflags & 0x8d5) == (state2.eflags & 0x8d5));

    assert(pw_x86_engine_destroy(&engine_res) == PW_OK);
    assert(pw_x86_engine_destroy(&engine_no_res) == PW_OK);
}

/* 11. A condition helper must preserve a resident ESP before a push. */
static void test_setcc_helper_preserves_resident_stack(void)
{
    /*
     *   cmp eax, ebx
     *   setne al          (calls the condition helper)
     *   push 2            (must use the original resident ESP)
     *   ret
     */
    uint8_t code[] = {0x39, 0xd8, 0x0f, 0x95, 0xc0, 0x6a, 0x02, 0xc3};
    TestSource src = {0x1000, code, sizeof(code)};
    PwVmBackend vm;
    PwX86Engine engine;
    PwX86CacheEntry entries[16];
    PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
    state.gpr[0] = 1;
    state.gpr[3] = 2;
    state.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;

    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_engine_init(&engine, &vm, entries, 16, 65536, 1,
                              test_source_view, &src) == PW_OK);
    assert(pw_x86_engine_set_residency(&engine, 1) == PW_OK);

    PwX86StepReport step;
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.eip == 2);
    assert(state.gpr[4] == 0x0300ff00);
    assert(*(uint32_t *)(uintptr_t)0x0300fefc == 2);

    const PwX86CacheEntry *entry = NULL;
    assert(pw_x86_cache_lookup(&engine.cache, 0x1000, &entry) == PW_OK);
    assert(entry->entry_contract.resident_mask & (1 << 4));

    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

/* 12. A matching successor must eventually spill inherited dirty values. */
static void test_matching_chain_preserves_inherited_dirty_value(void)
{
    uint8_t code[320];
    memset(code,0x90,sizeof(code));
    /* Block A: make EAX resident and dirty, then chain to 0x1080. */
    size_t a=0;
    code[a++]=0xb8;code[a++]=10;code[a++]=0;code[a++]=0;code[a++]=0;
    code[a++]=0x83;code[a++]=0xc0;code[a++]=1;
    code[a++]=0x83;code[a++]=0xc0;code[a++]=2;
    code[a++]=0xe9;
    int32_t delta=(int32_t)(0x1080-(0x1000+a+4));
    memcpy(code+a,&delta,4);a+=4;
    /* Block B: use but do not modify EAX.  Thirty-two CMPs terminate the
     * translated block through the bounded dynamic exit. */
    for(unsigned i=0;i<32;i++) {
        size_t p=0x80+i*5;
        code[p]=0x3d;code[p+1]=13;
    }

    TestSource src={0x1000,code,sizeof(code)};
    PwVmBackend vm;
    PwX86Engine engine;
    PwX86CacheEntry entries[32];
    PwX86State state={.eip=0x1080,.stack_low=0x03000000,.stack_high=0x03010000};
    state.gpr[4]=0x0300ff00;

    assert(pw_vm_posix_backend(&vm)==PW_OK);
    assert(pw_x86_engine_init(&engine,&vm,entries,32,131072,1,
                              test_source_view,&src)==PW_OK);
    assert(pw_x86_engine_set_chaining(&engine,1)==PW_OK);
    assert(pw_x86_engine_set_quantum(&engine,2)==PW_OK);

    /* Compile B first so A's forward edge is linked immediately. */
    PwX86StepReport step;
    assert(pw_x86_engine_step(&engine,&state,&step)==PW_OK);
    state.eip=0x1000;state.gpr[0]=0;
    assert(pw_x86_engine_step(&engine,&state,&step)==PW_OK);
    assert(state.eip==0x1120);
    assert(state.gpr[0]==13);
    assert(step.retired==36);
    assert(state.step_transitions==1);

    assert(pw_x86_engine_destroy(&engine)==PW_OK);
}

/*
 * 13. Base+index addressing with resident registers across a block boundary.
 *
 * This is the shape the loader's own rb-tree fixup runs - a load whose base is
 * one guest register and whose index is another, with the value that feeds the
 * index loaded earlier in the same block - and the block is entered from
 * another block, so the entry contract is what decides where those registers
 * live. It is the pattern the application-root run reaches when ntdll inserts
 * the module it has just mapped.
 *
 * This remains the compact regression for values passed from one canonical
 * block entry into a resident base/index consumer.
 */
static void test_base_index_load_across_blocks(void)
{
    /*
     * 0x1000: mov esi, 0x0300f000    ; the table, inside the mapped stack
     *         mov ebx, 0x11
     *         jmp 0x100f
     * 0x100f: mov ecx, [esi + 4]     ; ecx = 0x11
     *         xor eax, eax
     *         cmp ecx, ebx           ; equal, so the index below is 0
     *         setne al
     *         mov edi, eax
     *         mov eax, [esi + edi*4] ; eax = the table's first word
     *         ret
     */
    uint8_t code[] = {
        0xbe, 0x00, 0xf0, 0x00, 0x03,        /* mov esi, 0x0300f000 */
        0xbb, 0x11, 0x00, 0x00, 0x00,        /* mov ebx, 0x11 */
        0xe9, 0x00, 0x00, 0x00, 0x00,        /* jmp 0x100f */
        0x8b, 0x4e, 0x04,                    /* mov ecx, [esi + 4] */
        0x31, 0xc0,                          /* xor eax, eax */
        0x39, 0xd9,                          /* cmp ecx, ebx */
        0x0f, 0x95, 0xc0,                    /* setne al */
        0x89, 0xc7,                          /* mov edi, eax */
        0x8b, 0x04, 0xbe,                    /* mov eax, [esi + edi*4] */
        0xc3                                 /* ret */
    };
    TestSource src = {0x1000, code, sizeof(code)};
    PwVmBackend vm;
    PwX86Engine engine;
    PwX86CacheEntry entries[16];
    PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
    PwX86StepReport step;

    state.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;   /* the return address */
    *(uint32_t *)(uintptr_t)0x0300f000 = 0xaaaa0000;
    *(uint32_t *)(uintptr_t)0x0300f004 = 0x11;

    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_engine_init(&engine, &vm, entries, 16, 65536, 1,
                              test_source_view, &src) == PW_OK);
    /* The configuration the loader's fixup reaches: no chaining. */
    assert(pw_x86_engine_set_chaining(&engine, 0) == PW_OK);
    /* One step runs one block: the block that leaves the registers hot, then
     * the block whose load indexes with them. */
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.eip == 0x100f);
    assert(state.gpr[3] == 0x11);        /* what the first block left behind */
    assert(state.gpr[6] == 0x0300f000);
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.gpr[0] == 0xaaaa0000);
    assert(state.gpr[1] == 0x11);
    assert(state.gpr[6] == 0x0300f000);
    assert(state.gpr[7] == 0);
    assert(state.eip == 0x99999999);
    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

/*
 * 14. The block ntdll's rb-insert fixup actually runs, byte for byte, entered
 * canonically into a fresh engine.  EAX deliberately starts with non-zero high
 * bits: the guest XOR must clear the complete register before SETNE replaces
 * AL, even when EAX is resident and canonical memory is temporarily stale.
 */
static void test_rb_fixup_block_verbatim(void)
{
    /*
     * Taken from the staged ntdll at RVA 0x30aa7 (RtlRbInsertNodeEx+0x117):
     *   mov [ebp+0x14], edx
     *   mov ecx, [esi+4]
     *   xor eax, eax
     *   cmp ebx, ecx
     *   setne al
     *   mov edi, eax
     *   mov eax, [esi+edi*4]
     *   test eax, eax
     *   je +6
     */
    uint8_t code[] = {
        0x89, 0x55, 0x14,
        0x8b, 0x4e, 0x04,
        0x31, 0xc0,
        0x39, 0xcb,
        0x0f, 0x95, 0xc0,
        0x89, 0xc7,
        0x8b, 0x04, 0xbe,
        0x85, 0xc0,
        0x74, 0x06,
        0xc3
    };
    TestSource src = {0x1000, code, sizeof(code)};
    PwVmBackend vm;
    PwX86Engine engine;
    PwX86CacheEntry entries[16];
    PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
    PwX86StepReport step;

    state.gpr[5] = 0x0300ff00;          /* ebp */
    state.gpr[6] = 0x0300f000;          /* esi: the "node" */
    state.gpr[3] = 0x11;                /* ebx: the key */
    state.gpr[2] = 0x12345678;          /* edx: the value stored */
    state.gpr[0] = 0x10641580;          /* overwritten by xor eax, eax */
    *(uint32_t *)(uintptr_t)0x0300f004 = 0x11;
    *(uint32_t *)(uintptr_t)0x0300f000 = 0xaaaa0000;

    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_engine_init(&engine, &vm, entries, 16, 65536, 1,
                              test_source_view, &src) == PW_OK);
    assert(pw_x86_engine_set_chaining(&engine, 0) == PW_OK);
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.gpr[0] == 0xaaaa0000);
    assert(state.gpr[1] == 0x11);
    assert(*(uint32_t *)(uintptr_t)0x0300ff14 == 0x12345678);
    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

int main(void)
{
    PwVmBackend vm;
    PwVmRegion stack_region;
    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(vm.reserve_at(vm.context, 0x03000000, 65536, vm.page_bytes, &stack_region) == PW_OK);
    assert(vm.commit(vm.context, &stack_region, 0, stack_region.bytes, PW_PROT_READ | PW_PROT_WRITE) == PW_OK);

    test_load_once_write_many();
    test_all_eight_gprs_and_pressure();
    test_partial_register_aliasing();
    test_dirty_resident_byte_alu_aliasing();
    test_esp_operand_base_index_dest();
    test_fault_observes_latest_values();
    test_reconciliation_mismatched_contracts();
    test_chain_invalidation_canonical_fallback();
    test_deterministic_allocation_output();
    test_parity_residency_switch();
    test_setcc_helper_preserves_resident_stack();
    test_matching_chain_preserves_inherited_dirty_value();
    test_base_index_load_across_blocks();
    test_rb_fixup_block_verbatim();

    assert(vm.release(vm.context, &stack_region) == PW_OK);
    printf("all 14 register residency tests passed successfully\n");
    return 0;
}
