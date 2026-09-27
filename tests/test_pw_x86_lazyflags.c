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

/* 1. Producer chains defer canonical EFLAGS until the engine safepoint */
static void test_producer_chains_dead_flags(void)
{
    /*
     * Block A at 0x1000:
     *   mov eax, 10
     *   add eax, 5       (produces flags)
     *   jmp 0x1020
     *
     * Block B at 0x1020:
     *   mov ebx, 20
     *   add ebx, 15      (produces flags, previous flags not read)
     *   jmp 0x1040
     *
     * Block C at 0x1040:
     *   mov edx, 30
     *   ret              (no conditional branch or flag consumer)
     */
    uint8_t code[96];
    memset(code, 0x90, sizeof(code));

    /* Block A at 0x1000 (offset 0) */
    code[0] = 0xb8; code[1] = 10; code[2] = 0; code[3] = 0; code[4] = 0; /* mov eax, 10 */
    code[5] = 0x83; code[6] = 0xc0; code[7] = 5;                        /* add eax, 5 */
    code[8] = 0xe9; code[9] = 0x13; code[10] = 0; code[11] = 0; code[12] = 0; /* jmp 0x1020 (rel 0x13=19) */

    /* Block B at 0x1020 (offset 32) */
    code[32] = 0xbb; code[33] = 20; code[34] = 0; code[35] = 0; code[36] = 0; /* mov ebx, 20 */
    code[37] = 0x83; code[38] = 0xc3; code[39] = 15;                          /* add ebx, 15 */
    code[40] = 0xe9; code[41] = 0x13; code[42] = 0; code[43] = 0; code[44] = 0; /* jmp 0x1040 (rel 0x13=19) */

    /* Block C at 0x1040 (offset 64) */
    code[64] = 0xba; code[65] = 30; code[66] = 0; code[67] = 0; code[68] = 0; /* mov edx, 30 */
    code[69] = 0xc3;                                                            /* ret */

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
    assert(pw_x86_engine_set_lazy_flags(&engine, 1) == PW_OK);

    /* Prime cache */
    PwX86StepReport step;
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.eip == 0x1020);
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.eip == 0x1040);
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.eip == 0x99999999);

    /* Run chained */
    state.eip = 0x1000;
    state.gpr[0] = 0;
    state.gpr[3] = 0;
    state.gpr[2] = 0;
    state.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;

    uint64_t prev_commits = engine.flags_safepoint_commits;
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.eip == 0x99999999);
    assert(state.gpr[0] == 15);
    assert(state.gpr[3] == 35);
    assert(state.gpr[2] == 30);
    assert(engine.flags_safepoint_commits - prev_commits == 1);

    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

/* 2. Consumers of each individual arithmetic flag and composite conditions */
static void test_consumers_individual_and_composite_flags(void)
{
    PwVmBackend vm;
    assert(pw_vm_posix_backend(&vm) == PW_OK);

    /* Test 2.1: CF via add overflow and setc */
    {
        uint8_t code[] = {
            0xb8, 0xff, 0xff, 0xff, 0xff, /* mov eax, 0xffffffff */
            0x83, 0xc0, 0x01,             /* add eax, 1 (sets CF=1, ZF=1) */
            0x0f, 0x92, 0xc2,             /* setc dl (dl = 1) */
            0xc3                          /* ret */
        };
        TestSource src = {0x1000, code, sizeof(code)};
        PwX86Engine engine;
        PwX86CacheEntry entries[4];
        PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
        state.gpr[4] = 0x0300ff00;
        *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;
        assert(pw_x86_engine_init(&engine, &vm, entries, 4, 65536, 1, test_source_view, &src) == PW_OK);
        assert(pw_x86_engine_set_lazy_flags(&engine, 1) == PW_OK);
        PwX86StepReport step;
        assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
        assert((state.gpr[2] & 0xff) == 1);
        assert((state.eflags & 1) == 1); /* CF=1 */
        assert(pw_x86_engine_destroy(&engine) == PW_OK);
    }

    /* Test 2.2: ZF via sub and setz */
    {
        uint8_t code[] = {
            0xb8, 42, 0, 0, 0,            /* mov eax, 42 */
            0x83, 0xe8, 42,               /* sub eax, 42 (sets ZF=1) */
            0x0f, 0x94, 0xc2,             /* setz dl (dl = 1) */
            0xc3                          /* ret */
        };
        TestSource src = {0x1000, code, sizeof(code)};
        PwX86Engine engine;
        PwX86CacheEntry entries[4];
        PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
        state.gpr[4] = 0x0300ff00;
        *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;
        assert(pw_x86_engine_init(&engine, &vm, entries, 4, 65536, 1, test_source_view, &src) == PW_OK);
        assert(pw_x86_engine_set_lazy_flags(&engine, 1) == PW_OK);
        PwX86StepReport step;
        assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
        assert((state.gpr[2] & 0xff) == 1);
        assert((state.eflags & 0x40) != 0); /* ZF=1 */
        assert(pw_x86_engine_destroy(&engine) == PW_OK);
    }

    /* Test 2.3: SF via sub and sets */
    {
        uint8_t code[] = {
            0xb8, 5, 0, 0, 0,             /* mov eax, 5 */
            0x83, 0xe8, 10,               /* sub eax, 10 (eax = -5, sets SF=1) */
            0x0f, 0x98, 0xc2,             /* sets dl (dl = 1) */
            0xc3                          /* ret */
        };
        TestSource src = {0x1000, code, sizeof(code)};
        PwX86Engine engine;
        PwX86CacheEntry entries[4];
        PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
        state.gpr[4] = 0x0300ff00;
        *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;
        assert(pw_x86_engine_init(&engine, &vm, entries, 4, 65536, 1, test_source_view, &src) == PW_OK);
        assert(pw_x86_engine_set_lazy_flags(&engine, 1) == PW_OK);
        PwX86StepReport step;
        assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
        assert((state.gpr[2] & 0xff) == 1);
        assert((state.eflags & 0x80) != 0); /* SF=1 */
        assert(pw_x86_engine_destroy(&engine) == PW_OK);
    }

    /* Test 2.4: OF via signed overflow and seto */
    {
        uint8_t code[] = {
            0xb8, 0xff, 0xff, 0xff, 0x7f, /* mov eax, 0x7fffffff (INT32_MAX) */
            0x83, 0xc0, 1,                /* add eax, 1 (sets OF=1, SF=1) */
            0x0f, 0x90, 0xc2,             /* seto dl (dl = 1) */
            0xc3                          /* ret */
        };
        TestSource src = {0x1000, code, sizeof(code)};
        PwX86Engine engine;
        PwX86CacheEntry entries[4];
        PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
        state.gpr[4] = 0x0300ff00;
        *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;
        assert(pw_x86_engine_init(&engine, &vm, entries, 4, 65536, 1, test_source_view, &src) == PW_OK);
        assert(pw_x86_engine_set_lazy_flags(&engine, 1) == PW_OK);
        PwX86StepReport step;
        assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
        assert((state.gpr[2] & 0xff) == 1);
        assert((state.eflags & 0x800) != 0); /* OF=1 */
        assert(pw_x86_engine_destroy(&engine) == PW_OK);
    }

    /* Test 2.5: PF via parity and setp */
    {
        uint8_t code[] = {
            0xb8, 3, 0, 0, 0,             /* mov eax, 3 (binary: 00000011 -> even parity -> PF=1) */
            0x83, 0xc0, 0,                /* add eax, 0 */
            0x0f, 0x9a, 0xc2,             /* setp dl (dl = 1) */
            0xc3                          /* ret */
        };
        TestSource src = {0x1000, code, sizeof(code)};
        PwX86Engine engine;
        PwX86CacheEntry entries[4];
        PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
        state.gpr[4] = 0x0300ff00;
        *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;
        assert(pw_x86_engine_init(&engine, &vm, entries, 4, 65536, 1, test_source_view, &src) == PW_OK);
        assert(pw_x86_engine_set_lazy_flags(&engine, 1) == PW_OK);
        PwX86StepReport step;
        assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
        assert((state.gpr[2] & 0xff) == 1);
        assert((state.eflags & 4) != 0); /* PF=1 */
        assert(pw_x86_engine_destroy(&engine) == PW_OK);
    }

    /* Test 2.6: Composite conditions (JA/JNBE, JBE/JNA, JG/JNLE, JLE/JNG) via SETcc */
    {
        /* 2.6.1: SETA (Above: CF=0 and ZF=0) */
        uint8_t code_seta[] = {
            0x39, 0xd8,       /* cmp eax, ebx */
            0x0f, 0x97, 0xc2, /* seta dl */
            0xc3              /* ret */
        };
        TestSource src = {0x1000, code_seta, sizeof(code_seta)};
        PwX86Engine engine;
        PwX86CacheEntry entries[4];
        assert(pw_x86_engine_init(&engine, &vm, entries, 4, 65536, 1, test_source_view, &src) == PW_OK);
        assert(pw_x86_engine_set_lazy_flags(&engine, 1) == PW_OK);

        PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
        state.gpr[4] = 0x0300ff00;
        *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;
        state.gpr[0] = 100; /* eax */
        state.gpr[3] = 50;  /* ebx */
        PwX86StepReport step;
        assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
        assert((state.gpr[2] & 0xff) == 1); /* 100 > 50 -> seta dl = 1 */

        /* 100 not > 100 */
        assert(pw_x86_engine_reset(&engine, 2) == PW_OK);
        state.eip = 0x1000;
        state.gpr[0] = 100;
        state.gpr[3] = 100;
        state.gpr[2] = 0;
        state.gpr[4] = 0x0300ff00;
        *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;
        assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
        assert((state.gpr[2] & 0xff) == 0); /* 100 not > 100 -> seta dl = 0 */

        assert(pw_x86_engine_destroy(&engine) == PW_OK);

        /* 2.6.2: SETBE (Below or Equal: CF=1 or ZF=1) */
        uint8_t code_setbe[] = {
            0x39, 0xd8,       /* cmp eax, ebx */
            0x0f, 0x96, 0xc2, /* setbe dl */
            0xc3              /* ret */
        };
        TestSource src_be = {0x1000, code_setbe, sizeof(code_setbe)};
        assert(pw_x86_engine_init(&engine, &vm, entries, 4, 65536, 1, test_source_view, &src_be) == PW_OK);
        assert(pw_x86_engine_set_lazy_flags(&engine, 1) == PW_OK);

        state.eip = 0x1000;
        state.gpr[0] = 50;
        state.gpr[3] = 100;
        state.gpr[2] = 0;
        state.gpr[4] = 0x0300ff00;
        *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;
        assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
        assert((state.gpr[2] & 0xff) == 1); /* 50 <= 100 -> setbe dl = 1 */

        assert(pw_x86_engine_destroy(&engine) == PW_OK);

        /* 2.6.3: SETG (Signed Greater: ZF=0 and SF=OF) */
        uint8_t code_setg[] = {
            0x39, 0xd8,       /* cmp eax, ebx */
            0x0f, 0x9f, 0xc2, /* setg dl */
            0xc3              /* ret */
        };
        TestSource src_g = {0x1000, code_setg, sizeof(code_setg)};
        assert(pw_x86_engine_init(&engine, &vm, entries, 4, 65536, 1, test_source_view, &src_g) == PW_OK);
        assert(pw_x86_engine_set_lazy_flags(&engine, 1) == PW_OK);

        state.eip = 0x1000;
        state.gpr[0] = 50;
        state.gpr[3] = (uint32_t)(int32_t)-50;
        state.gpr[2] = 0;
        state.gpr[4] = 0x0300ff00;
        *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;
        assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
        assert((state.gpr[2] & 0xff) == 1); /* 50 > -50 -> setg dl = 1 */

        assert(pw_x86_engine_destroy(&engine) == PW_OK);

        /* 2.6.4: SETLE (Signed Less or Equal: ZF=1 or SF!=OF) */
        uint8_t code_setle[] = {
            0x39, 0xd8,       /* cmp eax, ebx */
            0x0f, 0x9e, 0xc2, /* setle dl */
            0xc3              /* ret */
        };
        TestSource src_le = {0x1000, code_setle, sizeof(code_setle)};
        assert(pw_x86_engine_init(&engine, &vm, entries, 4, 65536, 1, test_source_view, &src_le) == PW_OK);
        assert(pw_x86_engine_set_lazy_flags(&engine, 1) == PW_OK);

        state.eip = 0x1000;
        state.gpr[0] = (uint32_t)(int32_t)-50;
        state.gpr[3] = 50;
        state.gpr[2] = 0;
        state.gpr[4] = 0x0300ff00;
        *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;
        assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
        assert((state.gpr[2] & 0xff) == 1); /* -50 <= 50 -> setle dl = 1 */

        assert(pw_x86_engine_destroy(&engine) == PW_OK);
    }
}

/* 3. ADC/SBB carry input after one or several chained blocks */
static void test_adc_sbb_carry_across_chained_blocks(void)
{
    /*
     * Block A at 0x1000:
     *   mov eax, 0xffffffff
     *   add eax, 1           (sets CF=1)
     *   jmp 0x1020
     *
     * Block B at 0x1020:
     *   mov edx, 0
     *   adc edx, 0           (edx = edx + 0 + CF = 1, clears CF=0)
     *   jmp 0x1040
     *
     * Block C at 0x1040:
     *   mov ecx, 0
     *   sbb ecx, 0           (ecx = ecx - 0 - CF = 0)
     *   ret
     */
    uint8_t code[96];
    memset(code, 0x90, sizeof(code));

    /* Block A at 0x1000 (offset 0) */
    code[0] = 0xb8; code[1] = 0xff; code[2] = 0xff; code[3] = 0xff; code[4] = 0xff; /* mov eax, 0xffffffff */
    code[5] = 0x83; code[6] = 0xc0; code[7] = 1;                                    /* add eax, 1 */
    code[8] = 0xe9; code[9] = 0x13; code[10] = 0; code[11] = 0; code[12] = 0;     /* jmp 0x1020 (rel 19) */

    /* Block B at 0x1020 (offset 32) */
    code[32] = 0xba; code[33] = 0; code[34] = 0; code[35] = 0; code[36] = 0;       /* mov edx, 0 */
    code[37] = 0x83; code[38] = 0xd2; code[39] = 0;                                 /* adc edx, 0 */
    code[40] = 0xe9; code[41] = 0x13; code[42] = 0; code[43] = 0; code[44] = 0;     /* jmp 0x1040 (rel 19) */

    /* Block C at 0x1040 (offset 64) */
    code[64] = 0xb9; code[65] = 0; code[66] = 0; code[67] = 0; code[68] = 0;       /* mov ecx, 0 */
    code[69] = 0x83; code[70] = 0xd9; code[71] = 0;                                 /* sbb ecx, 0 */
    code[72] = 0xc3;                                                                /* ret */

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
    assert(pw_x86_engine_set_lazy_flags(&engine, 1) == PW_OK);

    /* Compile all blocks */
    PwX86StepReport step;
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.eip == 0x1020);
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.eip == 0x1040);
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.eip == 0x99999999);

    /* Run linked */
    state.eip = 0x1000;
    state.gpr[0] = 0;
    state.gpr[2] = 0;
    state.gpr[1] = 0;
    state.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;

    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.eip == 0x99999999);
    assert(state.gpr[2] == 1); /* edx from ADC */
    assert(state.gpr[1] == 0); /* ecx from SBB */

    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

/* 4. CMP/TEST feeding Jcc and SETcc across a linked boundary */
static void test_cmp_test_feeding_jcc_setcc_across_boundary(void)
{
    /*
     * Block A at 0x1000:
     *   cmp eax, 42
     *   jmp 0x1020
     *
     * Block B at 0x1020:
     *   sete dl
     *   jz 0x1040
     *   mov ecx, 10
     *   ret
     *
     * Block C at 0x1040:
     *   mov ecx, 20
     *   ret
     */
    uint8_t code[96];
    memset(code, 0x90, sizeof(code));

    /* Block A at 0x1000 */
    code[0] = 0x83; code[1] = 0xf8; code[2] = 42;                                   /* cmp eax, 42 */
    code[3] = 0xe9; code[4] = 0x18; code[5] = 0; code[6] = 0; code[7] = 0;         /* jmp 0x1020 (rel 24) */

    /* Block B at 0x1020 */
    code[32] = 0x0f; code[33] = 0x94; code[34] = 0xc2;                             /* sete dl */
    code[35] = 0x74; code[36] = 0x17;                                             /* jz 0x1040 (rel 23) */
    code[37] = 0xb9; code[38] = 10; code[39] = 0; code[40] = 0; code[41] = 0;     /* mov ecx, 10 */
    code[42] = 0xc3;                                                              /* ret */

    /* Block C at 0x1040 */
    code[64] = 0xb9; code[65] = 20; code[66] = 0; code[67] = 0; code[68] = 0;     /* mov ecx, 20 */
    code[69] = 0xc3;                                                              /* ret */

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
    assert(pw_x86_engine_set_lazy_flags(&engine, 1) == PW_OK);

    /* Test with eax == 42 (equal) */
    state.gpr[0] = 42;
    PwX86StepReport step;
    /* Step 1: 0x1000 -> 0x1020 */
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    /* Step 2: 0x1020 -> 0x1040 */
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    /* Step 3: 0x1040 -> ret */
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.eip == 0x99999999);
    assert((state.gpr[2] & 0xff) == 1); /* sete dl */
    assert(state.gpr[1] == 20);         /* jz branch taken */

    /* Test again linked with eax == 99 (not equal) */
    state.eip = 0x1000;
    state.gpr[0] = 99;
    state.gpr[1] = 0;
    state.gpr[2] = 0;
    state.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;

    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    if (state.eip == 0x1025) {
        assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    }
    assert(state.eip == 0x99999999);
    assert((state.gpr[2] & 0xff) == 0); /* sete dl */
    assert(state.gpr[1] == 10);         /* jz fallthrough */

    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

/* 5. INC/DEC preserved CF and NEG edge cases */
static void test_inc_dec_preserved_cf_and_neg_edges(void)
{
    PwVmBackend vm;
    assert(pw_vm_posix_backend(&vm) == PW_OK);

    /* 5.1: INC/DEC preserving CF */
    {
        /*
         * mov eax, 0xffffffff
         * add eax, 1            (sets CF=1)
         * inc ebx               (ebx = 1, CF MUST remain 1)
         * mov edx, 0
         * adc edx, 0            (edx = 0 + 0 + CF = 1)
         * dec ebx               (ebx = 0, CF from ADC is 0, MUST remain 0)
         * mov ecx, 0
         * adc ecx, 0            (ecx = 0 + 0 + CF = 0)
         * ret
         */
        uint8_t code[] = {
            0xb8, 0xff, 0xff, 0xff, 0xff, /* mov eax, 0xffffffff */
            0x83, 0xc0, 1,                /* add eax, 1 (CF=1) */
            0x43,                         /* inc ebx */
            0xba, 0, 0, 0, 0,             /* mov edx, 0 */
            0x83, 0xd2, 0,                /* adc edx, 0 */
            0x4b,                         /* dec ebx */
            0xb9, 0, 0, 0, 0,             /* mov ecx, 0 */
            0x83, 0xd1, 0,                /* adc ecx, 0 */
            0xc3                          /* ret */
        };
        TestSource src = {0x1000, code, sizeof(code)};
        PwX86Engine engine;
        PwX86CacheEntry entries[4];
        PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
        state.gpr[4] = 0x0300ff00;
        *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;
        assert(pw_x86_engine_init(&engine, &vm, entries, 4, 65536, 1, test_source_view, &src) == PW_OK);
        assert(pw_x86_engine_set_lazy_flags(&engine, 1) == PW_OK);

        PwX86StepReport step;
        assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
        assert(state.gpr[2] == 1); /* edx received CF=1 preserved across INC */
        assert(state.gpr[1] == 0); /* ecx received CF=0 preserved across DEC */
        assert(state.gpr[3] == 0); /* ebx inc then dec */

        assert(pw_x86_engine_destroy(&engine) == PW_OK);
    }

    /* 5.2: NEG edge cases */
    {
        /*
         * neg eax (eax == 0 -> CF=0, ZF=1, OF=0, SF=0)
         * ret
         */
        uint8_t code[] = {
            0xf7, 0xd8, /* neg eax */
            0xc3        /* ret */
        };
        TestSource src = {0x1000, code, sizeof(code)};
        PwX86Engine engine;
        PwX86CacheEntry entries[4];
        PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
        assert(pw_x86_engine_init(&engine, &vm, entries, 4, 65536, 1, test_source_view, &src) == PW_OK);
        assert(pw_x86_engine_set_lazy_flags(&engine, 1) == PW_OK);

        /* Case 1: NEG 0 */
        state.eip = 0x1000;
        state.gpr[0] = 0;
        state.gpr[4] = 0x0300ff00;
        *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;
        PwX86StepReport step;
        assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
        assert(state.gpr[0] == 0);
        assert((state.eflags & 1) == 0);     /* CF = 0 */
        assert((state.eflags & 0x40) != 0);  /* ZF = 1 */
        assert((state.eflags & 0x800) == 0); /* OF = 0 */

        /* Case 2: NEG 1 */
        assert(pw_x86_engine_reset(&engine, 2) == PW_OK);
        state.eip = 0x1000;
        state.gpr[0] = 1;
        state.gpr[4] = 0x0300ff00;
        *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;
        assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
        assert(state.gpr[0] == 0xffffffff);
        assert((state.eflags & 1) == 1);     /* CF = 1 */
        assert((state.eflags & 0x40) == 0);  /* ZF = 0 */
        assert((state.eflags & 0x80) != 0);  /* SF = 1 */

        /* Case 3: NEG 0x80000000 (INT32_MIN -> overflow!) */
        assert(pw_x86_engine_reset(&engine, 3) == PW_OK);
        state.eip = 0x1000;
        state.gpr[0] = 0x80000000;
        state.gpr[4] = 0x0300ff00;
        *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;
        assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
        assert(state.gpr[0] == 0x80000000);
        assert((state.eflags & 1) == 1);     /* CF = 1 */
        assert((state.eflags & 0x800) != 0); /* OF = 1 */

        assert(pw_x86_engine_destroy(&engine) == PW_OK);
    }
}

/* 6. Zero, one and multi-bit immediate/CL shifts */
static void test_shift_zero_one_multibit_cl(void)
{
    PwVmBackend vm;
    assert(pw_vm_posix_backend(&vm) == PW_OK);

    /* 6.1: Shift count zero preserves flags */
    {
        /*
         * mov eax, 42
         * cmp eax, 42       (sets ZF=1, CF=0, SF=0)
         * mov ecx, 0
         * shl eax, cl       (count is 0 -> flags preserved)
         * ret
         */
        uint8_t code[] = {
            0xb8, 42, 0, 0, 0, /* mov eax, 42 */
            0x83, 0xf8, 42,    /* cmp eax, 42 */
            0xb9, 0, 0, 0, 0,  /* mov ecx, 0 */
            0xd3, 0xe0,        /* shl eax, cl */
            0xc3               /* ret */
        };
        TestSource src = {0x1000, code, sizeof(code)};
        PwX86Engine engine;
        PwX86CacheEntry entries[4];
        PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
        state.gpr[4] = 0x0300ff00;
        *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;
        assert(pw_x86_engine_init(&engine, &vm, entries, 4, 65536, 1, test_source_view, &src) == PW_OK);
        assert(pw_x86_engine_set_lazy_flags(&engine, 1) == PW_OK);

        PwX86StepReport step;
        assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
        assert(state.gpr[0] == 42);
        assert((state.eflags & 0x40) != 0); /* ZF still 1 */
        assert((state.eflags & 1) == 0);    /* CF still 0 */

        assert(pw_x86_engine_destroy(&engine) == PW_OK);
    }

    /* 6.2: One-bit shift */
    {
        /*
         * mov eax, 0x80000000
         * shl eax, 1
         * ret
         */
        uint8_t code[] = {
            0xb8, 0, 0, 0, 0x80, /* mov eax, 0x80000000 */
            0xd1, 0xe0,          /* shl eax, 1 */
            0xc3                 /* ret */
        };
        TestSource src = {0x1000, code, sizeof(code)};
        PwX86Engine engine;
        PwX86CacheEntry entries[4];
        PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
        state.gpr[4] = 0x0300ff00;
        *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;
        assert(pw_x86_engine_init(&engine, &vm, entries, 4, 65536, 1, test_source_view, &src) == PW_OK);
        assert(pw_x86_engine_set_lazy_flags(&engine, 1) == PW_OK);

        PwX86StepReport step;
        assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
        assert(state.gpr[0] == 0);
        assert((state.eflags & 1) == 1);    /* CF = 1 */
        assert((state.eflags & 0x40) != 0); /* ZF = 1 */

        assert(pw_x86_engine_destroy(&engine) == PW_OK);
    }

    /* 6.3: Multi-bit immediate shift */
    {
        /*
         * mov eax, 0x10000000
         * shl eax, 4            (shifts out bit 28 into CF=1, result = 0)
         * ret
         */
        uint8_t code[] = {
            0xb8, 0, 0, 0, 0x10, /* mov eax, 0x10000000 */
            0xc1, 0xe0, 4,       /* shl eax, 4 */
            0xc3                 /* ret */
        };
        TestSource src = {0x1000, code, sizeof(code)};
        PwX86Engine engine;
        PwX86CacheEntry entries[4];
        PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
        state.gpr[4] = 0x0300ff00;
        *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;
        assert(pw_x86_engine_init(&engine, &vm, entries, 4, 65536, 1, test_source_view, &src) == PW_OK);
        assert(pw_x86_engine_set_lazy_flags(&engine, 1) == PW_OK);

        PwX86StepReport step;
        assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
        assert(state.gpr[0] == 0);
        assert((state.eflags & 1) == 1); /* CF = 1 */

        assert(pw_x86_engine_destroy(&engine) == PW_OK);
    }

    /* 6.4: An eager shift supersedes a pending arithmetic descriptor. */
    {
        uint8_t code[] = {
            0xb8, 0xff, 0xff, 0xff, 0xff, /* mov eax, -1 */
            0x83, 0xc0, 1,                /* add eax, 1: CF=1 */
            0xd1, 0xe8,                   /* shr eax, 1: CF=0, ZF=1 */
            0xc3
        };
        TestSource src = {0x1000, code, sizeof(code)};
        PwX86Engine engine;
        PwX86CacheEntry entries[4];
        PwX86State state = {
            .eip=0x1000,.stack_low=0x03000000,.stack_high=0x03010000
        };
        state.gpr[4]=0x0300ff00;
        *(uint32_t *)(uintptr_t)state.gpr[4]=0x99999999;
        assert(pw_x86_engine_init(&engine,&vm,entries,4,65536,1,
                                  test_source_view,&src)==PW_OK);
        assert(pw_x86_engine_set_lazy_flags(&engine,1)==PW_OK);
        PwX86StepReport step;
        assert(pw_x86_engine_step(&engine,&state,&step)==PW_OK);
        assert(state.gpr[0]==0);
        assert((state.eflags&1)==0);
        assert((state.eflags&0x40)!=0);
        assert(pw_x86_engine_destroy(&engine)==PW_OK);
    }
}

/* 7. Memory faults between a producer and would-be consumer */
static void test_memory_fault_preserves_prefault_flags(void)
{
    /*
     * 0x1000:
     *   mov eax, 5
     *   sub eax, 10          (eax = -5, sets SF=1, CF=1, ZF=0)
     *   mov edx, [0]         (faults at 0x1008: invalid memory 0)
     *   add eax, 1
     *   ret
     */
    uint8_t code[] = {
        0xb8, 5, 0, 0, 0,        /* mov eax, 5 */
        0x83, 0xe8, 10,          /* sub eax, 10 */
        0x8b, 0x15, 0, 0, 0, 0,  /* mov edx, [0] */
        0x83, 0xc0, 1,           /* add eax, 1 */
        0xc3
    };
    TestSource src = {0x1000, code, sizeof(code)};
    PwVmBackend vm;
    PwX86Engine engine;
    PwX86CacheEntry entries[4];
    PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
    state.gpr[4] = 0x0300ff00;

    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_engine_init(&engine, &vm, entries, 4, 65536, 1, test_source_view, &src) == PW_OK);
    assert(pw_x86_engine_set_lazy_flags(&engine, 1) == PW_OK);

    PwX86StepReport step;
    int status = pw_x86_engine_step(&engine, &state, &step);
    assert(status == PW_ERR_VM);
    assert(state.eip == 0x1008); /* faulting instruction PC */
    assert(state.gpr[0] == 0xfffffffb); /* -5 */
    /* Pre-fault flags must be preserved */
    assert((state.eflags & 0x80) != 0); /* SF = 1 */
    assert((state.eflags & 1) == 1);    /* CF = 1 */
    assert((state.eflags & 0x40) == 0); /* ZF = 0 */

    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

/* 8. Unmasked x87 trap, import/helper, chain-quantum and diagnostics exits */
static void test_unmasked_exits_commit_canonical_flags(void)
{
    PwVmBackend vm;
    assert(pw_vm_posix_backend(&vm) == PW_OK);

    /* 8.1: x87 trap exit commits canonical flags */
    {
        /*
         * mov eax, 5
         * sub eax, 10       (SF=1, CF=1)
         * fld1; fldz; fdivp (divide by zero trap)
         */
        uint8_t code[] = {
            0xb8, 5, 0, 0, 0,               /* mov eax, 5 */
            0x83, 0xe8, 10,                 /* sub eax, 10 */
            0xd9, 0xe8, 0xd9, 0xee, 0xde, 0xf9 /* fld1; fldz; fdivp */
        };
        TestSource src = {0x3000, code, sizeof(code)};
        PwX86Engine engine;
        PwX86CacheEntry entries[4];
        assert(pw_x86_engine_init(&engine, &vm, entries, 4, 65536, 1, test_source_view, &src) == PW_OK);
        assert(pw_x86_engine_set_lazy_flags(&engine, 1) == PW_OK);

        PwX86State state = {.eip = 0x3000};
        pw_guest_fp_init(&state.fp);
        state.fp.x87_control = (uint16_t)(state.fp.x87_control & ~4u); /* unmask divide-by-zero */

        PwX86StepReport step;
        int status = pw_x86_engine_step(&engine, &state, &step);
        assert(status == PW_ERR_X87_TRAP);
        assert((state.eflags & 0x80) != 0); /* SF=1 */
        assert((state.eflags & 1) == 1);    /* CF=1 */

        assert(pw_x86_engine_destroy(&engine) == PW_OK);
    }

    /* 8.2: Chain-quantum safepoint exit commits canonical flags */
    {
        /*
         * Loop at 0x1000:
         *   dec ecx
         *   jnz 0x1000
         *   ret
         */
        uint8_t code[] = {
            0x49,       /* dec ecx */
            0x75, 0xfd, /* jnz -3 (to 0x1000) */
            0xc3        /* ret */
        };
        TestSource src = {0x1000, code, sizeof(code)};
        PwX86Engine engine;
        PwX86CacheEntry entries[4];
        assert(pw_x86_engine_init(&engine, &vm, entries, 4, 65536, 1, test_source_view, &src) == PW_OK);
        assert(pw_x86_engine_set_chaining(&engine, 1) == PW_OK);
        assert(pw_x86_engine_set_quantum(&engine, 8) == PW_OK);
        assert(pw_x86_engine_set_lazy_flags(&engine, 1) == PW_OK);

        PwX86State state = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
        state.gpr[1] = 1000; /* ecx */
        state.gpr[4] = 0x0300ff00;
        *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;

        PwX86StepReport step;
        assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
        /* Exited via safepoint return */
        assert(state.eip == 0x1000);
        assert(state.gpr[1] < 1000);
        assert((state.eflags & 0x40) == 0); /* ZF=0 because ecx != 0 */
        assert(engine.safepoint_returns >= 1);

        assert(pw_x86_engine_destroy(&engine) == PW_OK);
    }
}

/* 9. Reconciliation with register residency and chain invalidation */
static void test_reconciliation_with_residency_and_invalidation(void)
{
    /*
     * Block A at 0x1000:
     *   mov eax, 10
     *   add eax, 20
     *   cmp eax, 30
     *   jmp 0x1020
     *
     * Block B at 0x1020:
     *   sete dl
     *   add ebx, 100
     *   ret
     */
    uint8_t code[64];
    memset(code, 0x90, sizeof(code));

    /* Block A */
    code[0] = 0xb8; code[1] = 10; code[2] = 0; code[3] = 0; code[4] = 0; /* mov eax, 10 */
    code[5] = 0x83; code[6] = 0xc0; code[7] = 20;                        /* add eax, 20 */
    code[8] = 0x83; code[9] = 0xf8; code[10] = 30;                       /* cmp eax, 30 */
    code[11] = 0xe9; code[12] = 0x10; code[13] = 0; code[14] = 0; code[15] = 0; /* jmp 0x1020 (rel 16) */

    /* Block B */
    code[32] = 0x0f; code[33] = 0x94; code[34] = 0xc2;                   /* sete dl */
    code[35] = 0x83; code[36] = 0xc3; code[37] = 100;                    /* add ebx, 100 */
    code[38] = 0xc3;                                                    /* ret */

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
    assert(pw_x86_engine_set_residency(&engine, 1) == PW_OK);
    assert(pw_x86_engine_set_lazy_flags(&engine, 1) == PW_OK);

    /* Compile both blocks */
    PwX86StepReport step;
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.eip == 0x1020);
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.eip == 0x99999999);
    assert((state.gpr[2] & 0xff) == 1);
    assert(state.gpr[3] == 100);

    /* Run together chained */
    state.eip = 0x1000;
    state.gpr[0] = 0;
    state.gpr[2] = 0;
    state.gpr[3] = 0;
    state.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.eip == 0x99999999);
    assert(state.gpr[0] == 30);
    assert((state.gpr[2] & 0xff) == 1);
    assert(state.gpr[3] == 100);

    /* Test cache invalidation and clean re-execution */
    assert(pw_x86_engine_reset(&engine, 2) == PW_OK);
    assert(engine.unlinks >= 1);

    state.eip = 0x1000;
    state.gpr[0] = 0;
    state.gpr[2] = 0;
    state.gpr[3] = 0;
    state.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state.gpr[4] = 0x99999999;
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.eip == 0x1020);
    assert(pw_x86_engine_step(&engine, &state, &step) == PW_OK);
    assert(state.eip == 0x99999999);
    assert(state.gpr[0] == 30);
    assert((state.gpr[2] & 0xff) == 1);
    assert(state.gpr[3] == 100);

    assert(pw_x86_engine_destroy(&engine) == PW_OK);
}

/* 10. Byte-memory and 16-bit register ADC import pending guest CF. */
static void test_adc_width_and_address_temporaries(void)
{
    uint8_t code[] = {
        0xb8, 0xff, 0xff, 0xff, 0xff, /* mov eax, -1 */
        0x83, 0xc0, 1,                /* add eax, 1: CF=1 */
        0x80, 0x54, 0x24, 0x04, 0,   /* adc byte [esp+4], 0 */
        0xb8, 0xff, 0xff, 0xff, 0xff, /* mov eax, -1 */
        0x83, 0xc0, 1,                /* add eax, 1: CF=1 */
        0xb9, 0, 0, 0, 0,             /* mov ecx, 0 */
        0xbb, 0, 0, 0, 0,             /* mov ebx, 0 */
        0x66, 0x11, 0xd9,             /* adc cx, bx */
        0xb8, 0xff, 0xff, 0xff, 0xff, /* mov eax, -1 */
        0x83, 0xc0, 1,                /* add eax, 1: CF=1 */
        0xb8, 0, 0, 0, 0,             /* mov eax, 0 (flags unchanged) */
        0x14, 0,                      /* adc al, 0 */
        0xc3
    };
    TestSource src = {0x1000, code, sizeof(code)};
    PwVmBackend vm;
    assert(pw_vm_posix_backend(&vm) == PW_OK);

    for(unsigned lazy=0;lazy<=1;lazy++) {
        PwX86Engine engine;
        PwX86CacheEntry entries[8];
        PwX86State state = {
            .eip=0x1000,.stack_low=0x03000000,.stack_high=0x03010000
        };
        state.gpr[4]=0x0300ff00;
        *(uint32_t *)(uintptr_t)state.gpr[4]=0x99999999;
        *(uint32_t *)(uintptr_t)(state.gpr[4]+4)=0;
        assert(pw_x86_engine_init(&engine,&vm,entries,8,65536,1,
                                  test_source_view,&src)==PW_OK);
        assert(pw_x86_engine_set_lazy_flags(&engine,lazy)==PW_OK);
        PwX86StepReport step;
        for(unsigned steps=0;state.eip>=0x1000 && state.eip<0x1000+sizeof(code);steps++) {
            assert(steps<8);
            int status=pw_x86_engine_step(&engine,&state,&step);
            if(status!=PW_OK)
                fprintf(stderr,"adc width debug lazy=%u eip=0x%08x status=%d\n",
                        lazy,state.eip,status);
            assert(status==PW_OK);
        }
        assert(state.eip==0x99999999);
        assert(*(uint8_t *)(uintptr_t)(0x0300ff04)==1);
        assert((state.gpr[1]&0xffff)==1);
        assert(state.gpr[0]==1);
        assert(pw_x86_engine_destroy(&engine)==PW_OK);
    }
}

/* 11. Parity with lazy flags disabled through test/runtime switch */
static void test_parity_lazy_flags_switch(void)
{
    /*
     * Sequence with mixed arithmetic, logical, branches, shifts, and inc/dec
     */
    uint8_t code[] = {
        0xb8, 100, 0, 0, 0,       /* mov eax, 100 */
        0xbb, 50, 0, 0, 0,        /* mov ebx, 50 */
        0x01, 0xd8,               /* add eax, ebx (150) */
        0x29, 0xd8,               /* sub eax, ebx (100) */
        0x83, 0xf8, 100,          /* cmp eax, 100 */
        0x74, 0x03,               /* jz +3 */
        0x83, 0xc0, 10,           /* add eax, 10 (skipped) */
        0x40,                     /* inc eax (101) */
        0x4b,                     /* dec ebx (49) */
        0xd1, 0xe0,               /* shl eax, 1 (202) */
        0x31, 0xc9,               /* xor ecx, ecx */
        0x83, 0xd1, 0,            /* adc ecx, 0 */
        0xc3                      /* ret */
    };
    TestSource src = {0x1000, code, sizeof(code)};
    PwVmBackend vm;
    PwX86Engine engine_lazy, engine_eager;
    PwX86CacheEntry entries1[16], entries2[16];
    PwX86State state1 = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
    PwX86State state2 = {.eip = 0x1000, .stack_low = 0x03000000, .stack_high = 0x03010000};
    state1.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state1.gpr[4] = 0x99999999;
    state2.gpr[4] = 0x0300ff00;
    *(uint32_t *)(uintptr_t)state2.gpr[4] = 0x99999999;

    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_engine_init(&engine_lazy, &vm, entries1, 16, 65536, 1, test_source_view, &src) == PW_OK);
    assert(pw_x86_engine_init(&engine_eager, &vm, entries2, 16, 65536, 1, test_source_view, &src) == PW_OK);
    assert(pw_x86_engine_set_lazy_flags(&engine_lazy, 1) == PW_OK);
    assert(pw_x86_engine_set_lazy_flags(&engine_eager, 0) == PW_OK);

    PwX86StepReport step1, step2;
    assert(pw_x86_engine_step(&engine_lazy, &state1, &step1) == PW_OK);
    assert(pw_x86_engine_step(&engine_eager, &state2, &step2) == PW_OK);

    assert(state1.eip == state2.eip);
    for (unsigned i = 0; i < 8; i++) {
        assert(state1.gpr[i] == state2.gpr[i]);
    }
    assert((state1.eflags & 0x8d5) == (state2.eflags & 0x8d5));

    assert(pw_x86_engine_destroy(&engine_lazy) == PW_OK);
    assert(pw_x86_engine_destroy(&engine_eager) == PW_OK);
}


/* 12. Every condition after every common producer, against the host CPU:
 * right after the producer (the branch tests the flags it captured), with an
 * instruction in between (the branch merges the pending flags first), with
 * the carry in both states, in all four modes. */
typedef uint64_t (*NativeFlags)(uint32_t a, uint32_t b, uint32_t carry);
#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
static uint64_t call_native(NativeFlags fn, uint32_t a, uint32_t b, uint32_t carry)
{
    return fn(a, b, carry);
}
typedef int (*BlockFn)(PwX86State *);
#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
static int call_block(BlockFn fn, PwX86State *state) { return fn(state); }

/* The flags a condition reads. */
static uint32_t branch_flags(unsigned condition)
{
    static const uint32_t flags[8] = { 0x800, 0x001, 0x040, 0x041, 0x080, 0x004, 0x880, 0x8c0 };
    return flags[condition >> 1];
}

static int condition_holds(unsigned condition, uint64_t f)
{
    const int cf = (f & 1) != 0, pf = (f & 4) != 0, zf = (f & 0x40) != 0, sf = (f & 0x80) != 0,
              of = (f & 0x800) != 0;
    int value;

    switch (condition >> 1) {
        case 0: value = of; break;
        case 1: value = cf; break;
        case 2: value = zf; break;
        case 3: value = cf || zf; break;
        case 4: value = sf; break;
        case 5: value = pf; break;
        case 6: value = sf != of; break;
        default: value = zf || sf != of; break;
    }
    return (condition & 1) ? !value : value;
}

static void test_jcc_against_native(void)
{
    /* defined: the flags the producer defines; the others keep their guest
     * value, which the host CPU does not model. */
    static const struct { uint8_t bytes[3]; uint8_t length; uint32_t defined; } producers[] = {
        { { 0x39, 0xc8 }, 2, 0x8c5 }, /* cmp eax, ecx */
        { { 0x29, 0xc8 }, 2, 0x8c5 }, /* sub eax, ecx */
        { { 0x01, 0xc8 }, 2, 0x8c5 }, /* add eax, ecx */
        { { 0x85, 0xc8 }, 2, 0x8c5 }, /* test eax, ecx */
        { { 0x21, 0xc8 }, 2, 0x8c5 }, /* and eax, ecx */
        { { 0x09, 0xc8 }, 2, 0x8c5 }, /* or eax, ecx */
        { { 0x31, 0xc8 }, 2, 0x8c5 }, /* xor eax, ecx */
        { { 0x11, 0xc8 }, 2, 0x8c5 }, /* adc eax, ecx */
        { { 0x19, 0xc8 }, 2, 0x8c5 }, /* sbb eax, ecx */
        { { 0xff, 0xc0 }, 2, 0x8c5 }, /* inc eax: CF is the incoming one */
        { { 0xff, 0xc8 }, 2, 0x8c5 }, /* dec eax */
        { { 0xf7, 0xd8 }, 2, 0x8c5 }, /* neg eax */
        { { 0xd1, 0xe0 }, 2, 0x8c5 }, /* shl eax, 1 */
        { { 0xd1, 0xf8 }, 2, 0x8c5 }, /* sar eax, 1 */
        { { 0x83, 0xf8, 0x05 }, 3, 0x8c5 }, /* cmp eax, 5 */
        { { 0xd1, 0xe8 }, 2, 0x8c5 }, /* shr eax, 1 */
        { { 0xc1, 0xe0, 0x03 }, 3, 0x0c5 }, /* shl eax, 3 */
        { { 0xc1, 0xe8, 0x1f }, 3, 0x0c5 }, /* shr eax, 31 */
        { { 0xc1, 0xf8, 0x03 }, 3, 0x0c5 }, /* sar eax, 3 */
        { { 0xd1, 0xc0 }, 2, 0x801 }, /* rol eax, 1 */
        { { 0xc1, 0xc0, 0x05 }, 3, 0x001 }, /* rol eax, 5 */
        { { 0xd1, 0xc8 }, 2, 0x801 }, /* ror eax, 1 */
        { { 0xc1, 0xc8, 0x07 }, 3, 0x001 }, /* ror eax, 7 */
    };
    static const uint32_t values[] = { 0, 1, 5, 0x7fffffffu, 0x80000000u, 0xfffffffeu,
                                       0xffffffffu };
    PwVmBackend vm;
    PwVmRegion native, code;
    PwX86Block block;
    unsigned checked = 0;

    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(vm.reserve(vm.context, 4096, vm.page_bytes, &native) == PW_OK);
    assert(vm.reserve(vm.context, 8192, vm.page_bytes, &code) == PW_OK);
    for (size_t p = 0; p < sizeof(producers) / sizeof(producers[0]); p++) {
        /* mov eax, edi; mov ecx, esi; bt edx, 0 (CF = carry); producer;
         * pushfq; pop rax; ret */
        uint8_t host[32];
        size_t n = 0;
        const uint8_t head[] = { 0x89, 0xf8, 0x89, 0xf1, 0x0f, 0xba, 0xe2, 0x00 };

        memcpy(host, head, sizeof(head)); n = sizeof(head);
        memcpy(host + n, producers[p].bytes, producers[p].length); n += producers[p].length;
        host[n++] = 0x9c; host[n++] = 0x58; host[n++] = 0xc3;
        assert(vm.commit(vm.context, &native, 0, native.bytes, PW_PROT_READ | PW_PROT_WRITE) ==
               PW_OK);
        memcpy(native.write_base, host, n);
        assert(vm.protect(vm.context, &native, 0, native.bytes, PW_PROT_READ | PW_PROT_EXEC) ==
               PW_OK);
        for (unsigned condition = 0; condition < 16; condition++)
            for (unsigned between = 0; between < 2; between++) {
                if (branch_flags(condition) & ~producers[p].defined) {
                    checked += 4u * 49u * 2u;
                    continue;
                }
                /* mov eax, A; mov ecx, B; [mov edx, 1;] producer; jcc +0x10 */
                uint8_t guest[32];
                size_t g = 0;

                guest[g++] = 0xb8; g += 4;
                guest[g++] = 0xb9; g += 4;
                if (between) { guest[g++] = 0xba; guest[g++] = 1; guest[g++] = 0; guest[g++] = 0;
                               guest[g++] = 0; }
                memcpy(guest + g, producers[p].bytes, producers[p].length);
                g += producers[p].length;
                guest[g++] = (uint8_t)(0x70 + condition);
                guest[g++] = 0x10;
                for (unsigned mode = 0; mode < 4; mode++) {
                    const PwX86TranslateOptions options = {
                        .residency_enabled = mode & 1, .lazy_flags_enabled = mode >> 1 };

                    assert(vm.commit(vm.context, &code, 0, code.bytes,
                                     PW_PROT_READ | PW_PROT_WRITE) == PW_OK);
                    for (size_t a = 0; a < sizeof(values) / sizeof(values[0]); a++)
                        for (size_t b = 0; b < sizeof(values) / sizeof(values[0]); b++) {
                            memcpy(guest + 1, &values[a], 4);
                            memcpy(guest + 6, &values[b], 4);
                            assert(vm.protect(vm.context, &code, 0, code.bytes,
                                              PW_PROT_READ | PW_PROT_WRITE) == PW_OK);
                            assert(pw_x86_translate_opts(guest, g, 0x2000, code.write_base,
                                                         code.bytes, &block, &options) == PW_OK);
                            assert(block.source_bytes == g);
                            assert(vm.protect(vm.context, &code, 0, code.bytes,
                                              PW_PROT_READ | PW_PROT_EXEC) == PW_OK);
                            for (uint32_t carry = 0; carry < 2; carry++) {
                                PwX86State state;
                                const uint64_t flags = call_native(
                                    (NativeFlags)native.exec_base, values[a], values[b], carry);
                                const uint32_t taken = 0x2000u + (uint32_t)g + 0x10u;

                                memset(&state, 0, sizeof(state));
                                state.eflags = 0x2u | carry;
                                state.chain_budget = 8;
                                assert(call_block((BlockFn)code.exec_base, &state) == 0);
                                pw_x86_commit_canonical_flags(&state);
                                if ((state.eip == taken) != condition_holds(condition, flags) ||
                                    ((state.eflags ^ (uint32_t)flags) & producers[p].defined)) {
                                    fprintf(stderr, "producer %zu condition %u between %u mode %u"
                                            " a=%08x b=%08x cf=%u eip=%08x eflags=%08x"
                                            " native=%08llx\n", p, condition, between, mode,
                                            values[a], values[b], carry, state.eip,
                                            state.eflags, (unsigned long long)flags);
                                    assert(0);
                                }
                                checked++;
                            }
                        }
                }
            }
    }
    assert(checked == 23u * 16u * 2u * 4u * 49u * 2u);
    assert(vm.release(vm.context, &code) == PW_OK);
    assert(vm.release(vm.context, &native) == PW_OK);
}

int main(void)
{
    PwVmBackend vm;
    PwVmRegion stack_region;
    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(vm.reserve_at(vm.context, 0x03000000, 65536, vm.page_bytes, &stack_region) == PW_OK);
    assert(vm.commit(vm.context, &stack_region, 0, stack_region.bytes, PW_PROT_READ | PW_PROT_WRITE) == PW_OK);

    test_producer_chains_dead_flags();
    test_consumers_individual_and_composite_flags();
    test_adc_sbb_carry_across_chained_blocks();
    test_cmp_test_feeding_jcc_setcc_across_boundary();
    test_inc_dec_preserved_cf_and_neg_edges();
    test_shift_zero_one_multibit_cl();
    test_memory_fault_preserves_prefault_flags();
    test_unmasked_exits_commit_canonical_flags();
    test_reconciliation_with_residency_and_invalidation();
    test_adc_width_and_address_temporaries();
    test_parity_lazy_flags_switch();
    test_jcc_against_native();

    assert(vm.release(vm.context, &stack_region) == PW_OK);
    printf("all 12 lazy arithmetic flags tests passed successfully\n");
    return 0;
}
