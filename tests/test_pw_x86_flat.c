/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * The flat memory guard (PwX86TranslateOptions.flat_low/high) must accept and
 * refuse exactly what the region-table guard does when the stack and the one
 * region are the flat range: every width, at and across both ends, for loads,
 * stores, read-modify-writes and the stack, in every engine mode.
 */
#include "../src/pw_x86_block.h"
#include "../src/pw_vm_posix.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { LOW = 0x05000000u, BYTES = 8192u, HIGH = LOW + BYTES };

typedef int (*BlockFn)(PwX86State *);
static PwVmBackend backend;
static PwVmRegion code, guest;
static PwX86State state;

#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
static int invoke(BlockFn fn, PwX86State *context) { return fn(context); }

typedef struct Outcome {
    int status;
    uint32_t gpr[8], eip, fault_address, fault_width, fault_write, word;
} Outcome;

static void reset_state(void)
{
    memset(&state, 0, sizeof(state));
    state.stack_low = LOW;
    state.stack_high = HIGH;
    state.memory_count = 1;
    state.memory[0].low = LOW;
    state.memory[0].high = HIGH;
    state.memory[0].permissions = PW_X86_READ | PW_X86_WRITE;
    state.eflags = 0x2;
    for (unsigned i = 0; i < 8; i++) state.gpr[i] = 0x01010101u * (i + 1);
    for (unsigned i = 0; i < BYTES; i++) ((uint8_t *)guest.write_base)[i] = (uint8_t)(i * 7u + 3u);
}

static Outcome run(const uint8_t *source, size_t bytes, unsigned flat, unsigned residency,
                   unsigned lazy, uint32_t ebx, uint32_t esp)
{
    const PwX86TranslateOptions options = { residency, lazy, NULL, 0, flat ? LOW : 0,
                                            flat ? HIGH : 0 };
    PwX86Block block;
    Outcome out;

    reset_state();
    state.gpr[3] = ebx;
    state.gpr[4] = esp;
    assert(backend.protect(NULL, &code, 0, code.bytes, PW_PROT_READ | PW_PROT_WRITE) == PW_OK);
    assert(pw_x86_translate_opts(source, bytes, 0x1000, code.write_base, code.bytes, &block,
                                 &options) == PW_OK);
    assert(block.source_bytes == bytes);
    assert(backend.protect(NULL, &code, 0, code.bytes, PW_PROT_READ | PW_PROT_EXEC) == PW_OK);
    out.status = invoke((BlockFn)code.exec_base, &state);
    pw_x86_commit_canonical_flags(&state);
    memcpy(out.gpr, state.gpr, sizeof(out.gpr));
    out.eip = state.eip;
    out.fault_address = state.fault_address;
    out.fault_width = state.fault_width;
    out.fault_write = state.fault_write;
    /* A write near either end shows up here. */
    memcpy(&out.word, (const uint8_t *)guest.write_base + BYTES - 4, 4);
    out.word ^= *(const uint32_t *)guest.write_base;
    return out;
}

static unsigned compared, accepted, refused;

/* flat on and off agree, in all four modes, for every address around both
 * ends of the range. */
static void same_everywhere(const uint8_t *source, size_t bytes, unsigned width, int stack)
{
    const uint32_t around[] = { LOW, LOW + 1, LOW - 1, LOW - width, HIGH - width,
                                HIGH - width + 1, HIGH - 1, HIGH, 0, 1, 0xffffffffu,
                                0xfffffff0u, LOW + BYTES / 2 };

    for (size_t a = 0; a < sizeof(around) / sizeof(around[0]); a++)
        for (unsigned mode = 0; mode < 4; mode++) {
            const uint32_t address = around[a];
            const uint32_t ebx = stack ? LOW + 64 : address;
            const uint32_t esp = stack ? address : HIGH - 64;
            Outcome table = run(source, bytes, 0, mode & 1, mode >> 1, ebx, esp);
            Outcome flat = run(source, bytes, 1, mode & 1, mode >> 1, ebx, esp);

            if (memcmp(&table, &flat, sizeof(table))) {
                fprintf(stderr, "flat guard differs: width %u address 0x%08x mode %u "
                        "status %d/%d fault 0x%x/0x%x\n", width, address, mode,
                        table.status, flat.status, table.fault_address, flat.fault_address);
                assert(0);
            }
            compared++;
            if (table.status == 0) accepted++; else refused++;
        }
}

/* Stops in the middle of a block name their own instruction, with the
 * effects of the instructions before them and none of the ones after, while
 * instructions that cannot stop store no EIP at all. */
static void stops_name_their_instruction(void)
{
    /* mov eax,5; add ecx,eax; inc edx; mov esi,[ebx]; add edi,3; push eax;
     * xor edx,ecx */
    static const uint8_t block_source[] = { 0xb8, 5, 0, 0, 0, 0x01, 0xc1, 0x42, 0x8b, 0x33,
                                            0x83, 0xc7, 3, 0x50, 0x31, 0xca };
    /* inc eax; div ecx */
    static const uint8_t divide[] = { 0x40, 0xf7, 0xf1 };
    const uint8_t eip_store[] = { 0xc7, 0x47, (uint8_t)offsetof(PwX86State, eip) };

    for (unsigned mode = 0; mode < 8; mode++) {
        const unsigned residency = mode & 1, lazy = (mode >> 1) & 1, flat = mode >> 2;
        Outcome out = run(block_source, sizeof(block_source), flat, residency, lazy, LOW + 64,
                          HIGH - 64);

        assert(out.status == 0 && out.eip == 0x1010 && out.gpr[0] == 5);
        assert(out.gpr[1] == 0x02020202u + 5 && out.gpr[7] == 0x08080808u + 3);
        assert(out.gpr[2] == ((0x03030303u + 1) ^ out.gpr[1]) && out.gpr[4] == HIGH - 68);
        /* The load at 0x1008 is refused. */
        out = run(block_source, sizeof(block_source), flat, residency, lazy, 0, HIGH - 64);
        assert(out.status == -1 && out.eip == 0x1008 && out.fault_address == 0);
        assert(out.gpr[0] == 5 && out.gpr[1] == 0x02020202u + 5 &&
               out.gpr[2] == 0x03030303u + 1 && out.gpr[6] == 0x07070707u &&
               out.gpr[7] == 0x08080808u);
        /* The push at 0x100d is refused: ESP is at the bottom of the range. */
        out = run(block_source, sizeof(block_source), flat, residency, lazy, LOW + 64, LOW);
        assert(out.status == -1 && out.eip == 0x100d && out.gpr[4] == LOW);
        assert(out.gpr[7] == 0x08080808u + 3 && out.gpr[2] == 0x03030303u + 1);
        /* A helper's failure: the divide by zero at 0x1001. */
        reset_state();
        {
            const PwX86TranslateOptions options = { residency, lazy, NULL, 0, flat ? LOW : 0,
                                                    flat ? HIGH : 0 };
            PwX86Block block;
            int status;

            assert(backend.protect(NULL, &code, 0, code.bytes, PW_PROT_READ | PW_PROT_WRITE) ==
                   PW_OK);
            assert(pw_x86_translate_opts(divide, sizeof(divide), 0x1000, code.write_base,
                                         code.bytes, &block, &options) == PW_OK &&
                   block.source_bytes == sizeof(divide));
            assert(backend.protect(NULL, &code, 0, code.bytes, PW_PROT_READ | PW_PROT_EXEC) ==
                   PW_OK);
            state.gpr[1] = 0;
            status = invoke((BlockFn)code.exec_base, &state);
            assert(status != 0 && state.eip == 0x1001 && state.gpr[0] == 0x01010101u + 1);
        }
    }
    /* Two instructions can stop (the load and the push), the last one leaves
     * its successor: at most those three stores and the exit's own. */
    {
        const PwX86TranslateOptions options = { 1, 1, NULL, 0, LOW, HIGH };
        uint8_t output[4096];
        PwX86Block block;
        unsigned stores = 0;

        assert(pw_x86_translate_opts(block_source, sizeof(block_source), 0x1000, output,
                                     sizeof(output), &block, &options) == PW_OK);
        for (size_t i = 0; i + sizeof(eip_store) <= block.code_bytes; i++)
            if (!memcmp(output + i, eip_store, sizeof(eip_store))) stores++;
        assert(stores >= 3 && stores <= 5);
    }
}

int main(void)
{
    /* mov eax,[ebx]; movzx eax,byte [ebx]; movzx eax,word [ebx]; mov [ebx],eax;
     * add [ebx],ecx (read-modify-write); movdqu xmm0,[ebx]; push eax; pop edx */
    static const uint8_t load32[] = { 0x8b, 0x03 };
    static const uint8_t load8[] = { 0x0f, 0xb6, 0x03 };
    static const uint8_t load16[] = { 0x0f, 0xb7, 0x03 };
    static const uint8_t store32[] = { 0x89, 0x03 };
    static const uint8_t rmw32[] = { 0x01, 0x0b };
    static const uint8_t load128[] = { 0xf3, 0x0f, 0x6f, 0x03 };
    static const uint8_t push[] = { 0x50 };
    static const uint8_t pop[] = { 0x5a };
    const PwX86TranslateOptions narrow = { 1, 1, NULL, 0, LOW, LOW + 15 };
    const PwX86TranslateOptions inverted = { 1, 1, NULL, 0, HIGH, LOW };
    PwX86Block block;
    uint8_t scratch[4096], reference[4096];
    size_t reference_bytes;

    assert(pw_vm_posix_backend(&backend) == PW_OK);
    assert(backend.reserve(NULL, 16384, 4096, &code) == PW_OK);
    assert(backend.reserve_at(NULL, LOW, BYTES, 4096, &guest) == PW_OK);
    assert(backend.commit(NULL, &guest, 0, guest.bytes, PW_PROT_READ | PW_PROT_WRITE) == PW_OK);

    same_everywhere(load32, sizeof(load32), 4, 0);
    same_everywhere(load8, sizeof(load8), 1, 0);
    same_everywhere(load16, sizeof(load16), 2, 0);
    same_everywhere(store32, sizeof(store32), 4, 0);
    same_everywhere(rmw32, sizeof(rmw32), 4, 0);
    same_everywhere(load128, sizeof(load128), 16, 0);
    same_everywhere(push, sizeof(push), 4, 1);
    same_everywhere(pop, sizeof(pop), 4, 1);
    assert(accepted && refused);
    stops_name_their_instruction();

    /* A refused access names itself, as the table guard does. */
    {
        Outcome out = run(load32, sizeof(load32), 1, 1, 1, HIGH - 3, HIGH - 64);
        assert(out.status == -1 && out.fault_address == HIGH - 3 && out.fault_width == 4 &&
               out.fault_write == 0);
        out = run(store32, sizeof(store32), 1, 0, 0, 0, HIGH - 64);
        assert(out.status == -1 && out.fault_address == 0 && out.fault_write == 1);
    }
    /* The guard is shorter: the point of the mode. */
    {
        const PwX86TranslateOptions table = { 1, 1, NULL, 0, 0, 0 };
        const PwX86TranslateOptions flat = { 1, 1, NULL, 0, LOW, HIGH };
        size_t flat_bytes;

        assert(pw_x86_translate_opts(load32, sizeof(load32), 0x1000, reference,
                                     sizeof(reference), &block, &table) == PW_OK);
        reference_bytes = block.code_bytes;
        assert(pw_x86_translate_opts(load32, sizeof(load32), 0x1000, scratch, sizeof(scratch),
                                     &block, &flat) == PW_OK);
        flat_bytes = block.code_bytes;
        assert(flat_bytes + 30 < reference_bytes);
    }
    /* A range under 16 bytes, or an inverted one, keeps the table guard. */
    assert(pw_x86_translate_opts(load32, sizeof(load32), 0x1000, scratch, sizeof(scratch),
                                 &block, &narrow) == PW_OK && block.code_bytes == reference_bytes);
    assert(pw_x86_translate_opts(load32, sizeof(load32), 0x1000, scratch, sizeof(scratch),
                                 &block, &inverted) == PW_OK && block.code_bytes == reference_bytes);
    printf("x86 flat guard passed: %u comparisons (%u accepted, %u refused), 8 forms, "
           "4 modes, both ends, fault record, shorter code, stops name their instruction\n", compared, accepted, refused);
    return 0;
}
