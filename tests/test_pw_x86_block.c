/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_x86_block.h"
#include "../src/pw_x86_padding.h"
#include "../src/pw_guest_call.h"
#include "../src/pw_vm_posix.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef int (*BlockFn)(PwX86State *);
static PwVmBackend backend;
static PwVmRegion code, stack;
static PwX86State state;
/* Generated code has no Clang function-type metadata before its entry.
 * Disable only that indirect-call check, not ASan or other UBSan checks. */
#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
static int invoke(BlockFn fn, PwX86State *context) { return fn(context); }
static int run(const uint8_t *source, size_t bytes, uint32_t pc)
{
    PwX86Block block;
    assert(backend.protect(NULL,&code,0,code.bytes,PW_PROT_READ|PW_PROT_WRITE)==PW_OK);
    assert(pw_x86_translate(source,bytes,pc,code.write_base,code.bytes,&block)==PW_OK);
    assert(block.source_bytes == bytes);
    assert(backend.protect(NULL,&code,0,code.bytes,PW_PROT_READ|PW_PROT_EXEC)==PW_OK);
    return invoke((BlockFn)code.exec_base,&state);
}
/*
 * Same as run(), but choosing the engine modes and committing pending flags
 * the way pw_x86_engine_step does. Every new instruction family is executed
 * through all four combinations: a mode-dependent result here is a defect,
 * and this is what caught a cmov branch offset that only mis-landed with
 * register residency disabled.
 */
static int run_mode(const uint8_t *source, size_t bytes, uint32_t pc,
                    unsigned residency, unsigned lazy_flags)
{
    PwX86Block block;
    int status;

    assert(backend.protect(NULL,&code,0,code.bytes,PW_PROT_READ|PW_PROT_WRITE)==PW_OK);
    status=pw_x86_translate_ext(source,bytes,pc,code.write_base,code.bytes,
                                &block,residency,lazy_flags);
    assert(status==PW_OK && block.source_bytes==bytes);
    assert(backend.protect(NULL,&code,0,code.bytes,PW_PROT_READ|PW_PROT_EXEC)==PW_OK);
    status=invoke((BlockFn)code.exec_base,&state);
    pw_x86_commit_canonical_flags(&state);
    return status;
}

/*
 * The form matrix the early review asked for: every accepted 0f opcode - which
 * is more than SSE, since the same space carries BT/BTS/BTR/BTC, CMOVcc and
 * BSF/BSR - in its register and its memory form, executed here through the
 * translator and, by tests/test_pw_sse_matrix.py, as native i386 assembled
 * from the same bytes with the same initial state.
 *
 * The oracle cannot be the instruction the candidate emits, because that is
 * what the translator executes. It is the same instruction assembled by a
 * different toolchain from the same bytes, run on the same CPU but with the
 * operands set up by an independent program; the whole state after the
 * instruction is compared byte for byte, so a wrong operand, direction, width,
 * condition or destination is visible even when the opcode is accepted.
 *
 * Three defects came out of this matrix: PMOVMSKB accepting a memory operand
 * the ISA does not have (a host sigill), the bit-test family validating the
 * base address while the CPU reads the bit-string unit, and CMOVcc with a
 * memory source moving unconditionally because the memory guard overwrote the
 * condition where it had been kept.
 *
 * The accepted set is discovered by asking the translator rather than by
 * repeating the decoder's tables here, so an opcode added to the decoder joins
 * the matrix and one that stops being accepted leaves it.
 */
enum {
    /* Fixed guest address of the comparison window, inside the page this
     * harness reserves for the guest stack. The native oracle maps the same
     * address, so EBX and every dereference are identical on both sides. */
    MATRIX_WINDOW = 0x03000800,
    MATRIX_WINDOW_BYTES = 64,
    MATRIX_XMM_BYTES = 128,
    MATRIX_GPR_BYTES = 32,
    /* GPRs, XMMs and the window. */
    MATRIX_STATE_BYTES = MATRIX_GPR_BYTES + MATRIX_XMM_BYTES +
                         MATRIX_WINDOW_BYTES,
    MATRIX_MAX_FORMS = 512,
};

static void matrix_init(uint8_t gprs[MATRIX_GPR_BYTES],
                        uint8_t xmm[MATRIX_XMM_BYTES],
                        uint8_t window[MATRIX_WINDOW_BYTES])
{
    for (unsigned index = 0; index < 8u; ++index) {
        /* Distinct patterns, so a swapped source and destination cannot look
         * right by symmetry. EBX is the window base; ESP is skipped by the
         * comparison because the native oracle runs on the process stack. */
        const uint32_t value = index == 3u ? MATRIX_WINDOW
                                           : 0x11111111u * (index + 1u);

        memcpy(&gprs[index * 4u], &value, 4u);
    }
    for (unsigned index = 0; index < MATRIX_XMM_BYTES; ++index)
        xmm[index] = (uint8_t)(0x10u + index);
    for (unsigned index = 0; index < MATRIX_WINDOW_BYTES; ++index)
        window[index] = (uint8_t)(0x80u + index);
}

/*
 * A form starts from the same state every time, and the only state it may see
 * is the initial state plus what its own instruction did. That means resetting
 * the whole context, not just the fields this file happens to set: an earlier
 * version kept process-level state between forms and produced a crash that
 * looked like a form's fault until the real cause - the bit-string guard
 * bypass - was found.
 */
static void matrix_load(const uint8_t *gprs, const uint8_t *xmm,
                        const uint8_t *window)
{
    const uint32_t stack_low = state.stack_low, stack_high = state.stack_high;

    memset(&state, 0, sizeof(state));
    state.stack_low = stack_low;
    state.stack_high = stack_high;
    state.gpr[4] = stack_high;
    memcpy(state.gpr, gprs, MATRIX_GPR_BYTES);
    memcpy(state.fp.xmm, xmm, MATRIX_XMM_BYTES);
    memcpy((void *)(uintptr_t)MATRIX_WINDOW, window, MATRIX_WINDOW_BYTES);
    state.memory_count = 1u;
    state.memory[0] = (PwX86Memory){ stack_low, stack_high,
                                     PW_X86_READ | PW_X86_WRITE };
}

static void matrix_read(uint8_t *out)
{
    memcpy(out, state.gpr, MATRIX_GPR_BYTES);
    memcpy(out + MATRIX_GPR_BYTES, state.fp.xmm, MATRIX_XMM_BYTES);
    memcpy(out + MATRIX_GPR_BYTES + MATRIX_XMM_BYTES,
           (void *)(uintptr_t)MATRIX_WINDOW, MATRIX_WINDOW_BYTES);
}

/*
 * The instruction bytes for one form: [prefix] 0f <opcode> <modrm> [imm].
 * form[3] is the length the *translator* consumed, not an assumption about the
 * encoding: some 0f opcodes (BSWAP among them) carry their operand in the
 * opcode byte and consume no ModRM, so the matrix records what the translator
 * measured and hands exactly those bytes to the native oracle.
 */
static unsigned matrix_source(const uint8_t form[4], uint8_t *source)
{
    const unsigned base = form[0] == 0u ? 0u : 1u;

    if (base)
        source[0] = form[0];
    source[base] = 0x0fu;
    source[base + 1u] = form[1];
    source[base + 2u] = form[2];
    source[base + 3u] = 0x03u;          /* imm8: a valid lane for all three */
    return form[3];
}

static unsigned matrix_forms(uint8_t forms[MATRIX_MAX_FORMS][4])
{
    static const uint8_t prefixes[4] = { 0x66u, 0xf3u, 0xf2u, 0x00u };
    uint8_t scratch[4096];
    unsigned count = 0u;

    for (unsigned prefix = 0; prefix < 4u; ++prefix)
        for (unsigned opcode = 0; opcode < 256u; ++opcode)
            for (unsigned memory = 0; memory < 2u; ++memory) {
                PwX86Block block;
                uint8_t form[4] = { prefixes[prefix], (uint8_t)opcode,
                                    memory ? 0x03u : 0xd3u, 0u };
                uint8_t source[5];
                const unsigned base = prefixes[prefix] ? 1u : 0u;
                unsigned length = base + 3u;    /* prefix 0f op modrm */
                int status;

                form[3] = (uint8_t)length;
                status = pw_x86_translate(source,
                                          matrix_source(form, source), 0x2000,
                                          scratch, sizeof(scratch), &block);
                if (status == PW_ERR_TRUNCATED) {
                    ++length;                   /* one immediate byte */
                    form[3] = (uint8_t)length;
                    status = pw_x86_translate(source,
                                              matrix_source(form, source),
                                              0x2000, scratch,
                                              sizeof(scratch), &block);
                }
                if (status != PW_OK)
                    continue;
                /* Trust the measured length: BSWAP and friends consume no
                 * ModRM, and the oracle must see exactly these bytes. */
                form[3] = (uint8_t)block.source_bytes;
                assert(count < MATRIX_MAX_FORMS);
                memcpy(forms[count++], form, sizeof(form));
            }
    return count;
}

static int sse_matrix(void)
{
    static uint8_t forms[MATRIX_MAX_FORMS][4];
    uint8_t gprs[MATRIX_GPR_BYTES], xmm[MATRIX_XMM_BYTES];
    uint8_t window[MATRIX_WINDOW_BYTES];
    uint8_t reference[MATRIX_STATE_BYTES];
    const unsigned count = matrix_forms(forms);

    assert(count > 0u);
    matrix_init(gprs, xmm, window);
    /* The generator replays this header, so the oracle starts from the
     * harness's own state rather than from a second copy that could drift. */
    assert(fwrite(gprs, 1u, sizeof(gprs), stdout) == sizeof(gprs));
    assert(fwrite(xmm, 1u, sizeof(xmm), stdout) == sizeof(xmm));
    assert(fwrite(window, 1u, sizeof(window), stdout) == sizeof(window));
    for (unsigned index = 0; index < count; ++index) {
        uint8_t source[5];
        uint8_t after[MATRIX_STATE_BYTES];
        uint8_t descriptor[5];
        const unsigned length = matrix_source(forms[index], source);
        int executed = 1;

        /* All four residency/lazy-flag combinations must agree: a mode
         * difference is a defect class this matrix is expected to catch. */
        for (unsigned mode = 0; mode < 4u; ++mode) {
            uint8_t probe_code[4096];
            PwX86Block probe;

            /* A form the default modes accept may still be refused under one
             * of the four combinations, and the harness must record that
             * rather than assert inside run_mode(). */
            if (pw_x86_translate_ext(source, length, 0x2000, probe_code,
                                     sizeof(probe_code), &probe, mode & 1u,
                                     mode >> 1) != PW_OK) {
                executed = 0;
                break;
            }
            matrix_load(gprs, xmm, window);
            if (run_mode(source, length, 0x2000, mode & 1u, mode >> 1) !=
                PW_OK) {
                /* The guard refused this operand: a form whose operands the
                 * initial state sends outside the declared region has no
                 * state to compare, and the native oracle must not run it
                 * either. Refusals are records too, so the set is visible. */
                executed = 0;
                break;
            }
            matrix_read(after);
            if (mode == 0u)
                memcpy(reference, after, sizeof(after));
            else
                assert(memcmp(reference, after, sizeof(after)) == 0);
        }
        memcpy(descriptor, forms[index], sizeof(forms[index]));
        descriptor[4] = (uint8_t)executed;
        assert(fwrite(descriptor, 1u, sizeof(descriptor), stdout) ==
               sizeof(descriptor));
        if (executed)
            assert(fwrite(reference, 1u, sizeof(reference), stdout) ==
                   sizeof(reference));
    }
    return 0;
}
static void addressing_tests(void)
{
    memset(state.gpr,0,sizeof(state.gpr));
    const uint8_t sound_index_address[]={0xba,1,0,0,0,0x0f,0xbf,0xfa,
        0x8d,0x34,0xbf,0xc1,0xe6,2};
    assert(run(sound_index_address,sizeof(sound_index_address),0x5f0)==PW_OK);
    assert(state.gpr[2]==1 && state.gpr[7]==1 && state.gpr[6]==20);
    /* Exercise every 32-bit SIB encoding in all memory displacement modes.
     * Expected arithmetic is C uint32_t, independent from emitted code. */
    for(unsigned mod=0;mod<3;mod++)for(unsigned rm=0;rm<8;rm++)
        for(unsigned sib=0;sib<(rm==4?256u:1u);sib++) {
            for(unsigned i=0;i<8;i++)state.gpr[i]=0xf0000100u+i*17;
            uint32_t before[8];memcpy(before,state.gpr,sizeof(before));
            uint8_t lea[7]={0x8d,(uint8_t)((mod<<6)|(3<<3)|rm)};
            size_t n=2;
            unsigned base=rm, index=4, scale=0;
            if(rm==4) {lea[n++]=(uint8_t)sib;base=sib&7;index=(sib>>3)&7;scale=sib>>6;}
            uint32_t value=(mod==0 && base==5)?0:before[base];
            if(rm==4 && index!=4)value+=before[index]<<scale;
            if(mod==1) {lea[n++]=0xf0;value-=16;}
            else if(mod==2 || (mod==0 && base==5)) {
                lea[n++]=3;lea[n++]=0;lea[n++]=0;lea[n++]=0x80;
                value+=0x80000003u;
            }
            assert(run(lea,n,0x600)==0);
            before[3]=value;
            assert(memcmp(before,state.gpr,sizeof(before))==0);
            assert(state.eip==0x600+n);
            /* Every proper prefix must be rejected as incomplete. */
            uint8_t out[512];PwX86Block block;
            for(size_t len=1;len<n;len++)
                assert(pw_x86_translate(lea,len,0,out,sizeof(out),&block)==PW_ERR_TRUNCATED);
        }
    state.gpr[4]=state.stack_high-64;
    state.gpr[0]=0x12345678;
    const uint8_t save[]={0x89,0x44,0x24,0xfc}; /* [esp-4] = eax */
    const uint8_t load[]={0x8b,0x6c,0x24,0xfc}; /* ebp = [esp-4] */
    assert(run(save,sizeof(save),0x700)==0);
    assert(run(load,sizeof(load),0x704)==0);
    assert(state.gpr[5]==0x12345678);
    /* Pinball's sprite registry uses MOV [ECX+EAX*4],EBX.  Cover the
     * indexed write itself, not only LEA's effective-address arithmetic. */
    state.gpr[0]=3;state.gpr[1]=state.stack_low+0x300;
    state.gpr[3]=0xdecafbad;
    const uint8_t indexed_store[]={0x89,0x1c,0x81};
    assert(run(indexed_store,sizeof(indexed_store),0x708)==PW_OK);
    assert(*(uint32_t *)(uintptr_t)(state.stack_low+0x30c)==0xdecafbad);
    /* Absolute disp32 is guest absolute, not host RIP-relative. */
    const uint8_t absolute[]={0x8b,0x15,0xbc,0x0f,0x00,0x03};
    assert(run(absolute,sizeof(absolute),0x710)==0);
    assert(state.gpr[2]==0x12345678);
    state.gpr[4]=state.stack_low;
    assert(run(save,sizeof(save),0x720)==-1 && state.eip==0x720);
    state.gpr[5]=0xabcddcba;
    assert(run(load,sizeof(load),0x730)==-1 && state.gpr[5]==0xabcddcba);
}
static void string_tests(void)
{
    uint8_t *memory=(uint8_t *)stack.write_base;
    const uint32_t source=state.stack_low+0x100,destination=state.stack_low+0x200;
    const uint8_t repeat_stosd[]={0xf3,0xab};
    memset(memory+0x200,0,32);state.gpr[0]=0x12345678;state.gpr[1]=4;
    state.gpr[7]=destination;state.eflags=0x202;
    assert(run(repeat_stosd,sizeof(repeat_stosd),0xc000)==0);
    for(unsigned i=0;i<4;i++)assert(((uint32_t *)(memory+0x200))[i]==0x12345678);
    assert(state.gpr[1]==0 && state.gpr[7]==destination+16);

    const uint8_t stosw[]={0x66,0xab};
    state.gpr[0]=0xaabbccdd;state.gpr[7]=destination+16;
    assert(run(stosw,sizeof(stosw),0xc010)==0);
    assert(*(uint16_t *)(memory+0x210)==0xccdd && state.gpr[7]==destination+18);
    const uint8_t repeat_stosw[]={0x66,0xf3,0xab};
    state.gpr[0]=0x1122beef;state.gpr[1]=3;state.gpr[7]=destination+18;
    assert(run(repeat_stosw,sizeof(repeat_stosw),0xc013)==0);
    for(unsigned i=0;i<3;i++)assert(*(uint16_t *)(memory+0x212+i*2)==0xbeef);
    assert(!state.gpr[1] && state.gpr[7]==destination+24);

    for(unsigned i=0;i<24;i++)memory[0x100+i]=(uint8_t)(0x40+i);
    memset(memory+0x200,0,24);state.gpr[1]=3;state.gpr[6]=source;
    state.gpr[7]=destination;state.eflags=0x202;
    const uint8_t repeat_movsd[]={0xf3,0xa5};
    assert(run(repeat_movsd,sizeof(repeat_movsd),0xc020)==0);
    assert(!memcmp(memory+0x100,memory+0x200,12));
    assert(state.gpr[1]==0 && state.gpr[6]==source+12 && state.gpr[7]==destination+12);

    /* DF walks high-to-low and still leaves SI/DI one element beyond the copy. */
    memset(memory+0x200,0,24);state.gpr[1]=5;state.gpr[6]=source+4;
    state.gpr[7]=destination+4;state.eflags=0x602;
    const uint8_t repeat_movsb[]={0xf3,0xa4};
    assert(run(repeat_movsb,sizeof(repeat_movsb),0xc030)==0);
    assert(!memcmp(memory+0x100,memory+0x200,5));
    assert(state.gpr[1]==0 && state.gpr[6]==source-1 && state.gpr[7]==destination-1);

    /* Zero count is a no-op; an out-of-range span faults atomically. */
    uint8_t before[24];memcpy(before,memory+0x200,sizeof(before));
    state.gpr[1]=0;state.gpr[6]=0;state.gpr[7]=0;state.eflags=0x202;
    assert(run(repeat_movsd,sizeof(repeat_movsd),0xc040)==0);
    assert(!memcmp(before,memory+0x200,sizeof(before)) && state.gpr[1]==0);

    uint32_t compare_left[]={1,2,3,4},compare_right[]={1,2,9,4};
    memcpy(memory+0x100,compare_left,sizeof(compare_left));
    memcpy(memory+0x200,compare_right,sizeof(compare_right));
    state.gpr[1]=4;state.gpr[6]=source;state.gpr[7]=destination;state.eflags=0x202;
    const uint8_t repeat_cmpsd[]={0xf3,0xa7};
    assert(run(repeat_cmpsd,sizeof(repeat_cmpsd),0xc045)==PW_OK);
    assert(state.gpr[1]==1 && state.gpr[6]==source+12 && state.gpr[7]==destination+12);
    assert(state.eflags==((0x202u&~0x8d5u)|0x95u));
    state.gpr[1]=0;state.gpr[6]=source;state.gpr[7]=destination;state.eflags=0xad7;
    assert(run(repeat_cmpsd,sizeof(repeat_cmpsd),0xc047)==PW_OK);
    assert(!state.gpr[1] && state.gpr[6]==source && state.gpr[7]==destination &&
           state.eflags==0xad7);

    state.gpr[0]=0xfeedface;state.gpr[1]=2;state.gpr[7]=state.stack_high-4;
    assert(run(repeat_stosd,sizeof(repeat_stosd),0xc050)==PW_ERR_VM);
    assert(state.gpr[1]==2 && state.gpr[7]==state.stack_high-4);

    uint8_t output[512];PwX86Block block;
    const uint8_t prefix[]={0xf3};
    assert(pw_x86_translate(prefix,sizeof(prefix),0,output,sizeof(output),&block)==PW_ERR_TRUNCATED);
    const uint8_t word_prefix[]={0x66,0x89};
    assert(pw_x86_translate(word_prefix,sizeof(word_prefix),0,output,sizeof(output),&block)==PW_ERR_TRUNCATED);
    const uint8_t repeat_word_prefix[]={0x66,0xf3};
    assert(pw_x86_translate(repeat_word_prefix,sizeof(repeat_word_prefix),0,output,sizeof(output),&block)==PW_ERR_TRUNCATED);
}
static void muldiv_tests(void)
{
    state.gpr[0]=0x40000000;state.gpr[2]=3;state.eflags=0x256;
    const uint8_t imul_eax_edx[]={0x0f,0xaf,0xc2};
    assert(run(imul_eax_edx,sizeof(imul_eax_edx),0xc0d0)==PW_OK &&
           state.gpr[0]==0xc0000000 && (state.eflags&0x801)==0x801);
    state.gpr[0]=0;state.gpr[1]=0x40000000;state.eflags=0x256;
    const uint8_t imul_ecx_imm32[]={0x69,0xc1,0x11,0x2b,0,0};
    assert(run(imul_ecx_imm32,sizeof(imul_ecx_imm32),0xc0e0)==PW_OK &&
           state.gpr[0]==0x40000000 && (state.eflags&0x801)==0x801 && (state.eflags&~0x801)==0x256);
    state.gpr[1]=(uint32_t)-7;state.eflags=0xad7;
    const uint8_t imul_ecx_imm8[]={0x6b,0xc1,0xfb};
    assert(run(imul_ecx_imm8,sizeof(imul_ecx_imm8),0xc0f0)==PW_OK &&
           state.gpr[0]==35 && !(state.eflags&0x801) && (state.eflags&~0x801)==(0xad7&~0x801));
    const uint8_t mul_ebx[]={0xf7,0xe3},imul_ebx[]={0xf7,0xeb};
    const uint8_t div_ebx[]={0xf7,0xf3},idiv_ebx[]={0xf7,0xfb};
    state.gpr[0]=0xffffffffu;state.gpr[2]=0x11223344;state.gpr[3]=2;state.eflags=0x246;
    assert(run(mul_ebx,sizeof(mul_ebx),0xc100)==PW_OK);
    assert(state.gpr[0]==0xfffffffeu && state.gpr[2]==1 && (state.eflags&0x801)==0x801);
    state.gpr[0]=0xffffffffu;state.gpr[2]=0;state.gpr[3]=2;state.eflags=0xa47;
    assert(run(imul_ebx,sizeof(imul_ebx),0xc110)==PW_OK);
    assert(state.gpr[0]==0xfffffffeu && state.gpr[2]==0xffffffffu && !(state.eflags&0x801));
    state.gpr[0]=0;state.gpr[2]=1;state.gpr[3]=3;
    assert(run(div_ebx,sizeof(div_ebx),0xc120)==PW_OK);
    assert(state.gpr[0]==0x55555555u && state.gpr[2]==1);
    state.gpr[0]=(uint32_t)-17;state.gpr[2]=UINT32_MAX;state.gpr[3]=5;
    assert(run(idiv_ebx,sizeof(idiv_ebx),0xc130)==PW_OK);
    assert((int32_t)state.gpr[0]==-3 && (int32_t)state.gpr[2]==-2);
    state.gpr[0]=7;state.gpr[2]=0;state.gpr[3]=0;uint32_t before[8];
    memcpy(before,state.gpr,sizeof(before));
    assert(run(div_ebx,sizeof(div_ebx),0xc140)==PW_ERR_VM);
    assert(!memcmp(before,state.gpr,sizeof(before)));
    state.gpr[0]=0;state.gpr[2]=1;state.gpr[3]=1;memcpy(before,state.gpr,sizeof(before));
    assert(run(idiv_ebx,sizeof(idiv_ebx),0xc150)==PW_ERR_VM);
    assert(!memcmp(before,state.gpr,sizeof(before)));
}
static void x87_transfer_tests(void)
{
    pw_guest_fp_init(&state.fp);state.gpr[0]=0xabcd0000;state.eflags=0xad7;
    uint32_t address=state.stack_low,bits=0x3f800000;
    memcpy((void *)(uintptr_t)address,&bits,4);
    uint8_t load_store[]={0xd9,0x05,(uint8_t)address,(uint8_t)(address>>8),
        (uint8_t)(address>>16),(uint8_t)(address>>24),
        0xd9,0x1d,(uint8_t)address,(uint8_t)(address>>8),
        (uint8_t)(address>>16),(uint8_t)(address>>24)};
    assert(run(load_store,sizeof(load_store),0xd400)==0);
    memcpy(&bits,(void *)(uintptr_t)address,4);
    assert(bits==0x3f800000 && pw_guest_x87_peek(&state.fp,0,(uint8_t[10]){0})==PW_ERR_NOT_FOUND);
    assert(state.eflags==0xad7 && state.eip==0xd400+sizeof(load_store));

    uint64_t double_bits=UINT64_C(0x400921fb54442d18),double_stored=0;
    memcpy((void *)(uintptr_t)address,&double_bits,8);
    uint8_t load_store64[]={0xdd,0x05,(uint8_t)address,(uint8_t)(address>>8),
        (uint8_t)(address>>16),(uint8_t)(address>>24),
        0xdd,0x15,(uint8_t)(address+8),(uint8_t)((address+8)>>8),
        (uint8_t)((address+8)>>16),(uint8_t)((address+8)>>24),
        0xdd,0x1d,(uint8_t)(address+16),(uint8_t)((address+16)>>8),
        (uint8_t)((address+16)>>16),(uint8_t)((address+16)>>24)};
    assert(run(load_store64,sizeof(load_store64),0xd410)==PW_OK);
    memcpy(&double_stored,(void *)(uintptr_t)(address+8),8);
    assert(double_stored==double_bits);
    memcpy(&double_stored,(void *)(uintptr_t)(address+16),8);
    assert(double_stored==double_bits &&
           pw_guest_x87_peek(&state.fp,0,(uint8_t[10]){0})==PW_ERR_NOT_FOUND);

    int32_t integer=-17;memcpy((void *)(uintptr_t)address,&integer,4);
    uint8_t fild[]={0xdb,0x05,(uint8_t)address,(uint8_t)(address>>8),
        (uint8_t)(address>>16),(uint8_t)(address>>24)};
    assert(run(fild,sizeof(fild),0xd420)==0);
    const uint8_t duplicate_status[]={0xd9,0xc0,0xdd,0xd9,0xdf,0xe0};
    assert(run(duplicate_status,sizeof(duplicate_status),0xd430)==0);
    assert((state.gpr[0]&0xffff)==state.fp.x87_status);
    uint8_t discarded[10];assert(pw_guest_x87_pop(&state.fp,discarded)==PW_OK);

    /* Captured Pinball physics sequence: FST ST(1), FSTP ST(0), then
     * FSTP m32real [ESP+8]. It must collapse the duplicate and store once. */
    bits=0x3f800000;memcpy((void *)(uintptr_t)address,&bits,4);
    const uint8_t load_one[]={0xd9,0x05,(uint8_t)address,(uint8_t)(address>>8),
        (uint8_t)(address>>16),(uint8_t)(address>>24)};
    assert(run(load_one,sizeof(load_one),0xd434)==PW_OK);
    const uint8_t duplicate_one[]={0xd9,0xc0};
    assert(run(duplicate_one,sizeof(duplicate_one),0xd438)==PW_OK);
    state.gpr[4]=state.stack_low+32;bits=0;
    const uint8_t captured[]={0xdd,0xd1,0xdd,0xd8,0xd9,0x5c,0x24,0x08};
    assert(run(captured,sizeof(captured),0xd43a)==PW_OK);
    memcpy(&bits,(void *)(uintptr_t)(state.gpr[4]+8),4);
    assert(bits==0x3f800000 &&
           pw_guest_x87_peek(&state.fp,0,(uint8_t[10]){0})==PW_ERR_NOT_FOUND);

    uint32_t two=0x40000000u,six=0x40c00000u,third=0;
    memcpy((void *)(uintptr_t)address,&six,4);
    memcpy((void *)(uintptr_t)(address+4),&two,4);
    uint8_t reverse_divide_pop[]={
        0xd9,0x05,(uint8_t)address,(uint8_t)(address>>8),
        (uint8_t)(address>>16),(uint8_t)(address>>24),
        0xd9,0x05,(uint8_t)(address+4),(uint8_t)((address+4)>>8),
        (uint8_t)((address+4)>>16),(uint8_t)((address+4)>>24),
        0xde,0xf1,
        0xd9,0x1d,(uint8_t)(address+8),(uint8_t)((address+8)>>8),
        (uint8_t)((address+8)>>16),(uint8_t)((address+8)>>24)};
    assert(run(reverse_divide_pop,sizeof(reverse_divide_pop),0xd438)==PW_OK);
    memcpy(&third,(void *)(uintptr_t)(address+8),4);
    assert(third==0x3eaaaaabu);

    const uint8_t constants[]={0xd9,0xe8,0xd9,0xee};
    assert(run(constants,sizeof(constants),0xd440)==0);
    uint8_t zero[10];assert(pw_guest_x87_pop(&state.fp,zero)==PW_OK);
    assert(!memcmp(zero,(uint8_t[10]){0},10));
    assert(pw_guest_x87_pop(&state.fp,discarded)==PW_OK);

    bits=0x3f800000;memcpy((void *)(uintptr_t)address,&bits,4);
    uint8_t sign_and_trig[]={0xd9,0x05,(uint8_t)address,(uint8_t)(address>>8),
        (uint8_t)(address>>16),(uint8_t)(address>>24),0xd9,0xe0,0xd9,0xfe,
        0xd9,0xff,0xd9,0x1d,(uint8_t)address,(uint8_t)(address>>8),
        (uint8_t)(address>>16),(uint8_t)(address>>24)};
    assert(run(sign_and_trig,sizeof(sign_and_trig),0xd448)==PW_OK);
    memcpy(&bits,(void *)(uintptr_t)address,4);
    assert((bits&0x7fffffffu)<=0x3f800000u);

    /* An eight-byte load that crosses the live boundary faults atomically. */
    state.gpr[1]=state.stack_high-7;PwGuestFp before=state.fp;
    const uint8_t bad[]={0xdd,0x01};
    assert(run(bad,sizeof(bad),0xd450)==-1 && state.eip==0xd450);
    assert(!memcmp(&before,&state.fp,sizeof(before)));
}
static void arithmetic_tests(void)
{
    const uint32_t signed_values[]={0,1,0x7fffffffu,0x80000000u,0xffffffffu};
    for(unsigned i=0;i<sizeof(signed_values)/sizeof(signed_values[0]);i++) {
        state.gpr[0]=signed_values[i];state.gpr[2]=0x12345678;state.eflags=0xad7;
        const uint8_t cdq[]={0x99};
        assert(run(cdq,sizeof(cdq),0x8e0)==0 && state.gpr[0]==signed_values[i] &&
               state.gpr[2]==(signed_values[i]&0x80000000u?UINT32_MAX:0) &&
               state.eflags==0xad7);
    }
    for(unsigned carry=0;carry<2;carry++) {
        state.gpr[0]=7;state.eflags=0x202|carry;
        const uint8_t sbb[]={0x1b,0xc0};
        assert(run(sbb,sizeof(sbb),0x8f0)==0);
        assert(state.gpr[0]==(carry?0xffffffffu:0));
        assert((state.eflags&1)==carry);
    }
    const uint32_t values[]={0,1,15,16,0x7fffffffu,0x80000000u,0xffffffffu};
    for(unsigned a=0;a<7;a++)for(unsigned b=0;b<7;b++)
        for(unsigned direction=0;direction<2;direction++) {
            uint32_t x=values[a],y=values[b],z=x-y;
            state.gpr[0]=x;state.gpr[1]=y;state.eflags=0x602;
            uint8_t sub[]={direction?0x2b:0x29,direction?0xc1:0xc8};
            assert(run(sub,2,0x900)==0 && state.gpr[0]==z && state.gpr[1]==y);
            uint32_t f=(x<y?1:0) | (z==0?0x40:0) | ((z>>31)?0x80:0);
            f|=((x^y^z)&16);
            f|=(((x^y)&(x^z))>>31)?0x800:0;
            unsigned parity=0;for(unsigned bit=0;bit<8;bit++)parity^=(z>>bit)&1;
            if(!parity)f|=4;
            assert(state.eflags==(0x602|f));
            state.gpr[0]=x;state.gpr[1]=y;state.eflags=0x612;
            const uint8_t logical[]={direction?0x33:0x31,direction?0xc1:0xc8};
            assert(run(logical,2,0x904)==0 && state.gpr[0]==(x^y));
            uint32_t v=x^y,lf=(v==0?0x40:0)|((v>>31)?0x80:0);
            unsigned lp=0;for(unsigned bit=0;bit<8;bit++)lp^=(v>>bit)&1;
            if(!lp)lf|=4;
            assert(state.eflags==(0x612|lf));
            uint32_t before_mov=state.eflags;
            const uint8_t mov[]={0xc7,0xc2,0x12,0x34,0x56,0x78};
            assert(run(mov,sizeof(mov),0x902)==0 && state.gpr[2]==0x78563412);
            assert(state.eflags==before_mov);
        }
    state.gpr[4]=state.stack_high-32;
    const uint8_t write[]={0xc7,0x44,0x24,4,0xff,0xff,0xff,0xff};
    assert(run(write,sizeof(write),0xa00)==0);
    assert(*(uint32_t *)(uintptr_t)(state.gpr[4]+4)==0xffffffffu);
    state.gpr[4]=state.stack_high-4;
    uint32_t flags=state.eflags;
    assert(run(write,sizeof(write),0xa10)==-1 && state.eip==0xa10);
    assert(state.eflags==flags);
}
static void shift_tests(void)
{
    const uint32_t values[]={0,1,0x80000000,0x7fffffff,0xffffffff,0x89abcdef};
    const unsigned kinds[]={4,5,7};
    for(unsigned k=0;k<3;k++)for(unsigned form=0;form<3;form++)
    for(unsigned memory=0;memory<2;memory++)for(unsigned mode=0;mode<4;mode++)
    for(unsigned count=0;count<256;count+=(mode?5:1))for(unsigned v=0;v<6;v++) {
        unsigned actual=form==1?1:count,masked=actual&31;
        uint32_t expected=values[v];unsigned long flags;
        if(k==0)__asm__ volatile("shll %%cl,%0; pushfq; popq %1":"+a"(expected),"=r"(flags):"c"(actual):"cc");
        else if(k==1)__asm__ volatile("shrl %%cl,%0; pushfq; popq %1":"+a"(expected),"=r"(flags):"c"(actual):"cc");
        else __asm__ volatile("sarl %%cl,%0; pushfq; popq %1":"+a"(expected),"=r"(flags):"c"(actual):"cc");
        state.gpr[0]=values[v];state.gpr[1]=count;state.gpr[2]=state.stack_high-4;state.eflags=0xad7;
        memcpy((void *)(uintptr_t)state.gpr[2],&values[v],4);
        const uint8_t op[]={(uint8_t)(form==0?0xc1:form==1?0xd1:0xd3),(uint8_t)((memory?2:0xc0)|(kinds[k]<<3)),(uint8_t)count};
        assert(run_mode(op,form==0?3:2,0xd100,mode&1,mode>>1)==0);
        uint32_t result=state.gpr[0];if(memory)memcpy(&result,(void *)(uintptr_t)state.gpr[2],4);
        unsigned mask=masked?(masked==1?0x8c5:0xc5):0;
        assert(result==expected && state.eflags==((0xad7&~mask)|((unsigned)flags&mask)));
        assert(state.gpr[1]==count && state.gpr[2]==state.stack_high-4);
    }
    /* Destination ECX must use its old CL. A following instruction must run. */
    state.gpr[1]=0x80000021;state.eflags=0x202;
    const uint8_t alias[]={0xd3,0xe9,0xb8,42,0,0,0};
    assert(run(alias,sizeof(alias),0xd200)==0 && state.gpr[1]==0x40000010 && state.gpr[0]==42);
    for(unsigned count=0;count<2;count++) {
        state.gpr[2]=state.stack_high-3;state.eflags=0xad7;
        const uint8_t bad[]={0xc1,0x22,(uint8_t)count};
        assert(run(bad,3,0xd300)==-1 && state.eip==0xd300 && state.eflags==0xad7);
    }
}
/*
 * ROL/ROR r/m32 against the host instruction: imm8, 1 and CL forms, register
 * and memory, every count 0..255, under all four engine modes. Only CF is
 * defined, and OF for a count of one; SF, ZF, AF and PF are left alone.
 */
static void rotate_tests(void)
{
    const uint32_t values[]={0,1,0x80000000,0x7fffffff,0xffffffff,0x89abcdef};
    for(unsigned right=0;right<2;right++)for(unsigned form=0;form<3;form++)
    for(unsigned memory=0;memory<2;memory++)for(unsigned mode=0;mode<4;mode++)
    for(unsigned count=0;count<256;count+=(mode?5:1))for(unsigned v=0;v<6;v++) {
        unsigned actual=form==1?1:count,masked=actual&31;
        uint32_t expected=values[v];unsigned long flags;
        if(right)__asm__ volatile("rorl %%cl,%0; pushfq; popq %1":"+a"(expected),"=r"(flags):"c"(actual):"cc");
        else __asm__ volatile("roll %%cl,%0; pushfq; popq %1":"+a"(expected),"=r"(flags):"c"(actual):"cc");
        state.gpr[0]=values[v];state.gpr[1]=count;state.gpr[2]=state.stack_high-4;state.eflags=0xad7;
        memcpy((void *)(uintptr_t)state.gpr[2],&values[v],4);
        const uint8_t op[]={(uint8_t)(form==0?0xc1:form==1?0xd1:0xd3),(uint8_t)((memory?2:0xc0)|(right<<3)),(uint8_t)count};
        assert(run_mode(op,form==0?3:2,0xd180,mode&1,mode>>1)==0);
        uint32_t result=state.gpr[0];if(memory)memcpy(&result,(void *)(uintptr_t)state.gpr[2],4);
        unsigned mask=masked?(masked==1?0x801:0x001):0;
        assert(result==expected && state.eflags==((0xad7&~mask)|((unsigned)flags&mask)));
        assert(state.gpr[1]==count && state.gpr[2]==state.stack_high-4);
    }
    /* A rotate between a producer and its consumer keeps the producer's ZF:
     * xor eax, eax; ror ecx, 16; setz dl. */
    for(unsigned mode=0;mode<4;mode++) {
        state.gpr[1]=0x12345678;state.gpr[2]=0;state.eflags=0x202;
        const uint8_t keep_zf[]={0x31,0xc0,0xc1,0xc9,16,0x0f,0x94,0xc2};
        assert(run_mode(keep_zf,sizeof(keep_zf),0xd1c0,mode&1,mode>>1)==0);
        assert(state.gpr[1]==0x56781234 && (state.gpr[2]&0xff)==1);
    }
    /* RCL/RCR (/2, /3) by a constant, and the 16-bit rotates by a constant,
     * against the host instruction with the carry in both states: register
     * and memory, imm8 and one, every count 0..255 (masked to five bits).
     * RCL/RCR by CL still stop the block at the instruction. */
    for(unsigned kind=0;kind<6;kind++)for(unsigned form=0;form<2;form++)
    for(unsigned memory=0;memory<2;memory++)for(unsigned mode=0;mode<4;mode++)
    for(unsigned count=0;count<256;count+=(mode?7:1))for(unsigned v=0;v<6;v++)
    for(unsigned carry=0;carry<2;carry++) {
        /* kind: rcl, rcr (32-bit); rol, ror, rcl, rcr (16-bit) */
        const unsigned word=kind>=2, group=kind<2?2+kind:kind-2;
        unsigned actual=form==1?1:count,masked=actual&31;
        uint32_t expected=values[v];unsigned long flags;
        if(kind==0)__asm__ volatile("btl $0,%k2; rcll %%cl,%0; pushfq; popq %1":"+a"(expected),"=r"(flags):"r"(carry),"c"(actual):"cc");
        else if(kind==1)__asm__ volatile("btl $0,%k2; rcrl %%cl,%0; pushfq; popq %1":"+a"(expected),"=r"(flags):"r"(carry),"c"(actual):"cc");
        else if(kind==2)__asm__ volatile("btl $0,%k2; rolw %%cl,%w0; pushfq; popq %1":"+a"(expected),"=r"(flags):"r"(carry),"c"(actual):"cc");
        else if(kind==3)__asm__ volatile("btl $0,%k2; rorw %%cl,%w0; pushfq; popq %1":"+a"(expected),"=r"(flags):"r"(carry),"c"(actual):"cc");
        else if(kind==4)__asm__ volatile("btl $0,%k2; rclw %%cl,%w0; pushfq; popq %1":"+a"(expected),"=r"(flags):"r"(carry),"c"(actual):"cc");
        else __asm__ volatile("btl $0,%k2; rcrw %%cl,%w0; pushfq; popq %1":"+a"(expected),"=r"(flags):"r"(carry),"c"(actual):"cc");
        const uint32_t initial=0xad6u|carry;
        state.gpr[0]=values[v];state.gpr[1]=count;state.gpr[2]=state.stack_high-4;state.eflags=initial;
        memcpy((void *)(uintptr_t)state.gpr[2],&values[v],4);
        uint8_t op[4];size_t n=0;
        if(word)op[n++]=0x66;
        op[n++]=(uint8_t)(form==0?0xc1:0xd1);
        op[n++]=(uint8_t)((memory?2:0xc0)|(group<<3));
        if(form==0)op[n++]=(uint8_t)count;
        assert(run_mode(op,n,0xd1e0,mode&1,mode>>1)==0);
        uint32_t result=state.gpr[0];if(memory)memcpy(&result,(void *)(uintptr_t)state.gpr[2],4);
        unsigned mask=masked?(masked==1?0x801:0x001):0;
        assert(result==expected && state.eflags==((initial&~mask)|((unsigned)flags&mask)));
        assert(state.gpr[1]==count && state.gpr[2]==state.stack_high-4);
    }
    PwX86Block block;uint8_t scratch[512];
    const uint8_t rcl_cl[]={0xd3,0xd0}, rcl_cl16[]={0x66,0xd3,0xd0};
    assert(pw_x86_translate(rcl_cl,sizeof(rcl_cl),0xd1e0,scratch,sizeof(scratch),&block)==PW_ERR_UNSUPPORTED);
    assert(pw_x86_translate(rcl_cl16,sizeof(rcl_cl16),0xd1e0,scratch,sizeof(scratch),&block)==PW_ERR_UNSUPPORTED);
}
/*
 * SHLD/SHRD against the host instruction: every count 0..255 (masked to five
 * bits), register and memory destinations, imm8 and CL forms, under all four
 * engine modes. Zero preserves every flag and only a count of one defines OF;
 * AF is never defined.
 */
static void double_shift_tests(void)
{
    const uint32_t values[][2]={{0,0},{1,0x80000000u},{0x89abcdefu,0x01234567u},
                                {0xffffffffu,0},{0x80000001u,0xfffffffeu}};
    for(unsigned left=0;left<2;left++)for(unsigned cl=0;cl<2;cl++)
    for(unsigned memory=0;memory<2;memory++)for(unsigned mode=0;mode<4;mode++)
    for(unsigned count=0;count<256;count+=(mode?7:1))for(unsigned v=0;v<5;v++) {
        uint32_t expected=values[v][0];unsigned long flags;
        unsigned masked=count&31;
        if(left)__asm__ volatile("shldl %%cl,%2,%0; pushfq; popq %1":"+r"(expected),"=r"(flags):"r"(values[v][1]),"c"(count):"cc");
        else __asm__ volatile("shrdl %%cl,%2,%0; pushfq; popq %1":"+r"(expected),"=r"(flags):"r"(values[v][1]),"c"(count):"cc");
        /* EAX or [EDX] is the destination, ESI the source, ECX the count. */
        state.gpr[0]=values[v][0];state.gpr[6]=values[v][1];state.gpr[1]=count;
        state.gpr[2]=state.stack_high-4;state.eflags=0xad7;
        memcpy((void *)(uintptr_t)state.gpr[2],&values[v][0],4);
        const uint8_t op[]={0x0f,(uint8_t)((left?0xa4:0xac)|cl),
                            (uint8_t)((memory?0x02:0xc0)|(6<<3)),(uint8_t)count};
        assert(run_mode(op,cl?3:4,0xd400,mode&1,mode>>1)==0);
        uint32_t result=state.gpr[0];if(memory)memcpy(&result,(void *)(uintptr_t)state.gpr[2],4);
        unsigned mask=masked?(masked==1?0x8c5:0xc5):0;
        assert(result==expected);
        if(memory)assert(state.gpr[0]==values[v][0]);
        assert(state.eflags==((0xad7&~mask)|((unsigned)flags&mask)));
        assert(state.gpr[6]==values[v][1] && state.gpr[1]==count && state.gpr[2]==state.stack_high-4);
    }
    /* The source and the destination may be the same register, and the count
     * register may be the source: each reads its value before the shift. */
    state.gpr[1]=0x80000004u;state.eflags=0x202;
    const uint8_t rotate_like[]={0x0f,0xa5,0xc9};      /* shld ecx, ecx, cl */
    assert(run(rotate_like,sizeof(rotate_like),0xd480)==0 && state.gpr[1]==0x00000048u);
    /* A block continues after the instruction. */
    state.gpr[0]=1;state.gpr[3]=0x80000000u;
    const uint8_t then_mov[]={0x0f,0xa4,0xd8,1,0xb9,42,0,0,0}; /* shld eax,ebx,1; mov ecx,42 */
    assert(run(then_mov,sizeof(then_mov),0xd490)==0 && state.gpr[0]==3 && state.gpr[1]==42);
    /* A destination the guard refuses faults at the instruction, flags intact. */
    state.gpr[2]=state.stack_high-3;state.eflags=0xad7;
    const uint8_t bad[]={0x0f,0xac,0x32,4};
    assert(run(bad,sizeof(bad),0xd4a0)==-1 && state.eip==0xd4a0 && state.eflags==0xad7);
    /* The imm8 form needs its immediate. */
    PwX86Block block;uint8_t scratch[512];
    const uint8_t truncated[]={0x0f,0xa4,0xd8};
    assert(pw_x86_translate(truncated,sizeof(truncated),0xd4b0,scratch,sizeof(scratch),&block)==PW_ERR_TRUNCATED);
}
static void extension_tests(void)
{
    const uint32_t values[]={0,1,0x7f,0x80,0xff,0x7fff,0x8000,0xffff};
    for(unsigned word=0;word<2;word++)for(unsigned sign=0;sign<2;sign++)
    for(unsigned src=0;src<8;src++)for(unsigned dst=0;dst<8;dst++)for(unsigned v=0;v<8;v++) {
        for(unsigned i=0;i<8;i++)state.gpr[i]=0xaabbccdd;
        unsigned reg=word?src:src&3,shift=word?0:(src>>2)*8,mask=word?0xffff:0xff;
        state.gpr[reg]=(state.gpr[reg]&~(mask<<shift))|((values[v]&mask)<<shift);
        uint32_t before[8];memcpy(before,state.gpr,sizeof(before));state.eflags=0xad7;
        uint32_t expected=values[v]&mask;if(sign && (expected&(word?0x8000:0x80)))expected|=~mask;
        const uint8_t extend[]={0x0f,(uint8_t)(0xb6+word+sign*8),(uint8_t)(0xc0|(dst<<3)|src)};
        assert(run(extend,3,0xd000)==0 && state.eflags==0xad7);
        before[dst]=expected;assert(!memcmp(before,state.gpr,sizeof(before)));
    }
    for(unsigned word=0;word<2;word++)for(unsigned sign=0;sign<2;sign++) {
        state.gpr[1]=state.stack_high-(word?2:1);state.eflags=0xad7;
        uint16_t value=word?0x8000:0x80;memcpy((void *)(uintptr_t)state.gpr[1],&value,word?2:1);
        const uint8_t op[]={0x0f,(uint8_t)(0xb6+word+sign*8),0x01};
        assert(run(op,3,0xd010)==0 && state.gpr[0]==(sign?(word?0xffff8000:0xffffff80):value) && state.eflags==0xad7);
        state.gpr[1]++;uint32_t before=state.gpr[0];
        assert(run(op,3,0xd020)==-1 && state.gpr[0]==before && state.eflags==0xad7);
    }
    for(unsigned sign=0;sign<2;sign++) {
        state.gpr[1]=0xaabbcc80;state.gpr[2]=0x11223344;state.eflags=0xad7;
        const uint8_t op[]={0x66,0x0f,(uint8_t)(sign?0xbe:0xb6),0xd1};
        assert(run(op,sizeof(op),0xd030)==0 && state.gpr[2]==(sign?0x1122ff80:0x11220080) &&
               state.eflags==0xad7);
    }
    {
        const uint8_t partial[]={0x66,0x0f};uint8_t output[128];PwX86Block block;
        assert(pw_x86_translate(partial,sizeof(partial),0xd040,output,sizeof(output),&block)==PW_ERR_TRUNCATED);
    }
}
static void ret_cleanup_tests(void)
{
    for(unsigned dec=0;dec<2;dec++)for(unsigned memory=0;memory<2;memory++)for(unsigned carry=0;carry<2;carry++) {
        uint32_t value=dec?0x80000000:0x7fffffff;
        state.gpr[0]=value;state.gpr[1]=state.stack_low;state.eflags=0x202|carry;
        memcpy(stack.write_base,&value,4);
        const uint8_t incdec[]={0xff,(uint8_t)((memory?1:0xc0)|(dec<<3)),0x90};
        assert(run(incdec,3,0xc010)==0 && state.eip==0xc013);
        uint32_t after=state.gpr[0];if(memory)memcpy(&after,stack.write_base,4);
        assert(after==(dec?0x7fffffff:0x80000000));
        assert(state.eflags==((dec?0xa16u:0xa96u)|carry));
    }
    const unsigned pops[]={0,1,4,8,12,13,255,65535};
    for(unsigned i=0;i<sizeof(pops)/sizeof(pops[0]);i++) {
        state.gpr[4]=state.stack_high-16;state.eflags=0xad7;
        uint32_t target=0x1001234;memcpy((void *)(uintptr_t)state.gpr[4],&target,4);
        const uint8_t ret[]={0xc2,(uint8_t)pops[i],(uint8_t)(pops[i]>>8)};
        assert(run(ret,3,0xc000)==(pops[i]<=12?0:-1));
        assert(state.eflags==0xad7);
        assert(state.gpr[4]==state.stack_high-16+(pops[i]<=12?4+pops[i]:0));
        assert(state.eip==(pops[i]<=12?target:0xc000));
    }
    state.gpr[4]=state.stack_high-16;state.eip=0x1003000;
    PwX86State caller=state;PwGuestCallback callback={0};uint32_t args[]={17,23};
    assert(pw_guest_callback_enter(&callback,&state,0x1004000,0xf1000020,args,2,PW_GUEST_STDCALL)==PW_OK);
    const uint8_t guest[]={0xb8,42,0,0,0,0xc2,8,0};
    assert(run(guest,sizeof(guest),0x1004000)==0 && state.eip==0xf1000020);
    uint64_t result;assert(pw_guest_callback_leave(&callback,32,&result)==PW_OK && result==42);
    assert(!memcmp(&caller,&state,sizeof(state)));
}
static void byte_tests(void)
{
    state.gpr[0]=0x112233f9;state.gpr[1]=0xaabbcc0f;state.eflags=0x202;
    const uint8_t add_al_cl[]={0x02,0xc1};
    assert(run(add_al_cl,sizeof(add_al_cl),0xaff0)==PW_OK);
    assert(state.gpr[0]==0x11223308 && (state.eflags&0x8d5)==0x11);
    state.gpr[0]=0x1122ff44;state.gpr[1]=0x55667700;state.eflags=0x203;
    const uint8_t adc_ah_ch[]={0x12,0xe5};
    assert(run(adc_ah_ch,sizeof(adc_ah_ch),0xaff2)==PW_OK);
    assert(state.gpr[0]==0x11227744 && (state.eflags&0x8d5)==0x15);
    state.gpr[0]=0x12345603;state.gpr[1]=state.stack_low+32;state.eflags=0x202;
    *((uint8_t *)stack.write_base+32)=5;
    const uint8_t sub_memory_al[]={0x28,0x01};
    assert(run(sub_memory_al,sizeof(sub_memory_al),0xaff4)==PW_OK);
    assert(*((uint8_t *)stack.write_base+32)==2 && state.gpr[0]==0x12345603);
    const uint8_t cmp_al_memory[]={0x3a,0x01};state.eflags=0x202;
    assert(run(cmp_al_memory,sizeof(cmp_al_memory),0xaff6)==PW_OK);
    assert((state.eflags&0x8d5)==0 && *((uint8_t *)stack.write_base+32)==2);
    for(unsigned src=0;src<8;src++)for(unsigned dst=0;dst<8;dst++)for(unsigned direction=0;direction<2;direction++) {
        for(unsigned i=0;i<4;i++)state.gpr[i]=0x778899aau+i*0x1101;
        uint32_t before[8];memcpy(before,state.gpr,sizeof(before));state.eflags=0xad7;
        unsigned shift=(dst>>2)*8,source_shift=(src>>2)*8;
        uint32_t value=(before[src&3]>>source_shift)&255;
        const uint8_t mov[]={direction?0x8a:0x88,(uint8_t)(0xc0|((direction?dst:src)<<3)|(direction?src:dst))};
        assert(run(mov,2,0xb000)==0 && state.eflags==0xad7);
        before[dst&3]=(before[dst&3]&~(255u<<shift))|(value<<shift);
        assert(!memcmp(before,state.gpr,sizeof(before)));
    }
    for(unsigned reg=0;reg<8;reg++) {
        unsigned shift=(reg>>2)*8;state.gpr[reg&3]=0x11223344;
        const uint8_t imm[]={(uint8_t)(0xb0+reg),0xfe};
        assert(run(imm,2,0xb010)==0 && state.gpr[reg&3]==((0x11223344u&~(255u<<shift))|(254u<<shift)));
        const uint8_t cmp[]={0x80,(uint8_t)(0xf8|reg),0xfe};state.eflags=0x202;
        assert(run(cmp,3,0xb020)==0 && state.eflags==0x246);
        state.eflags=0xad7;
        const uint8_t test_imm[]={0xf6,(uint8_t)(0xc0|reg),1};
        assert(run(test_imm,3,0xb021)==0 && state.eflags==0x256);
    }
    state.gpr[0]=0x11223380;state.eflags=0xad7;
    const uint8_t test_al[]={0xa8,0xff};
    assert(run(test_al,2,0xb022)==0 && state.gpr[0]==0x11223380 && state.eflags==0x292);
    state.gpr[0]=0x112233f1;state.eflags=0xad7;
    const uint8_t and_al[]={0x24,0x7f};
    assert(run(and_al,2,0xb024)==0 && state.gpr[0]==0x11223371 &&
           (state.eflags&0x8c5)==4);
    state.gpr[1]=state.stack_high-1;state.gpr[0]=0x11228044;state.eflags=0xad7;
    const uint8_t store[]={0x88,0x21},load[]={0x8a,0x01}; /* AH -> [ECX], [ECX] -> AL */
    assert(run(store,2,0xb030)==0 && *((uint8_t *)stack.write_base+stack.bytes-1)==0x80);
    const uint8_t test_memory[]={0xf6,0x01,0x80};
    assert(run(test_memory,3,0xb031)==0 && state.eflags==0x292);
    state.eflags=0xad7;
    assert(run(load,2,0xb040)==0 && state.gpr[0]==0x11228080 && state.eflags==0xad7);
    const uint8_t cmp_mem[]={0x80,0x39,0x7f};
    assert(run(cmp_mem,3,0xb050)==0 && state.eflags==0xa12); /* -128 - 127 overflows */
    const uint8_t immediate_store[]={0xc6,0x01,0x7f};
    assert(run(immediate_store,3,0xb060)==0 && *((uint8_t *)stack.write_base+stack.bytes-1)==0x7f);
    const uint8_t immediate_add[]={0x80,0x01,0xd0};state.eflags=0x202;
    assert(run(immediate_add,3,0xb065)==0 &&
           *((uint8_t *)stack.write_base+stack.bytes-1)==0x4f && (state.eflags&0x8d5)==1);
    uint8_t immediate_or_absolute[]={0x80,0x0d,0,0,0,0,0x80};
    uint32_t absolute=(uint32_t)state.gpr[1];memcpy(immediate_or_absolute+2,&absolute,4);
    *((uint8_t *)stack.write_base+stack.bytes-1)=1;state.eflags=0xad7;
    assert(run(immediate_or_absolute,sizeof(immediate_or_absolute),0xb067)==PW_OK &&
           *((uint8_t *)stack.write_base+stack.bytes-1)==0x81 && (state.eflags&0x8c5)==0x84);
    const uint8_t immediate_and[]={0x80,0x21,0x0f};
    assert(run(immediate_and,sizeof(immediate_and),0xb06e)==PW_OK &&
           *((uint8_t *)stack.write_base+stack.bytes-1)==1);
    const uint8_t immediate_sub[]={0x80,0x29,2};
    assert(run(immediate_sub,sizeof(immediate_sub),0xb071)==PW_OK &&
           *((uint8_t *)stack.write_base+stack.bytes-1)==0xff);
    const uint8_t immediate_xor[]={0x80,0x31,0x0f};
    assert(run(immediate_xor,sizeof(immediate_xor),0xb074)==PW_OK &&
           *((uint8_t *)stack.write_base+stack.bytes-1)==0xf0);
    *((uint8_t *)stack.write_base+stack.bytes-1)=0x4f;
    const uint8_t reverse[]={0x3a,0x01};
    assert(run(reverse,2,0xb070)==0 && state.eflags==0xa12);
    const uint8_t direct[]={0x38,0x01};
    assert(run(direct,2,0xb080)==0 && state.eflags==0xa87);
    const uint8_t test[]={0x84,0x01};
    assert(run(test,2,0xb090)==0 && state.eflags==0x246);
    state.gpr[1]=state.stack_high;uint32_t old=state.gpr[0];
    assert(run(load,2,0xb0a0)==-1 && state.gpr[0]==old && state.eflags==0x246);
    for(unsigned dec=0;dec<2;dec++)for(unsigned carry=0;carry<2;carry++) {
        state.gpr[6]=dec?0x80000000:0x7fffffff;state.eflags=0x202|carry;
        const uint8_t op[]={(uint8_t)(dec?0x4e:0x46)};
        assert(run(op,1,0xb0b0)==0 && state.gpr[6]==(dec?0x7fffffff:0x80000000));
        assert(state.eflags==((dec?0xa16u:0xa96u)|carry));
    }
}
static void unary_leave_tests(void)
{
    for(unsigned memory=0;memory<2;memory++)for(unsigned negate=0;negate<2;negate++) {
        uint32_t value=0x80000000;memcpy(stack.write_base,&value,4);
        state.gpr[0]=value;state.gpr[1]=state.stack_low;state.eflags=0xad7;
        const uint8_t op[]={0xf7,(uint8_t)((memory?1:0xc0)|((negate?3:2)<<3))};
        assert(run(op,2,0xa000)==0);
        uint32_t result=state.gpr[0];if(memory)memcpy(&result,stack.write_base,4);
        assert(result==(negate?0x80000000:0x7fffffff));
        assert(state.eflags==(negate?0xa87:0xad7));
    }
    uint32_t words[]={0x12345678,0x1002000};
    uint32_t frame=state.stack_high-8;memcpy((void *)(uintptr_t)frame,words,8);
    state.gpr[5]=frame;state.gpr[4]=state.stack_low;state.eflags=0xad7;
    const uint8_t epilogue[]={0xc9,0xc3};
    assert(run(epilogue,2,0xa010)==0 && state.gpr[4]==state.stack_high && state.gpr[5]==words[0] && state.eip==words[1] && state.eflags==0xad7);
    state.gpr[5]=state.stack_high-3;uint32_t esp=state.gpr[4];
    const uint8_t leave[]={0xc9};
    assert(run(leave,1,0xa020)==-1 && state.gpr[4]==esp && state.gpr[5]==state.stack_high-3 && state.eflags==0xad7);
}
static void arithmetic_memory_tests(void)
{
    const unsigned opcodes[]={0x01,0x03,0x29,0x2b,0x31,0x33};
    const uint32_t answers[]={8,8,2,0xfffffffe,6,6};
    const uint32_t flags[]={0x202,0x202,0x202,0x293,0x216,0x216};
    for(unsigned i=0;i<6;i++) {
        uint32_t value=5;memcpy(stack.write_base,&value,4);
        state.gpr[0]=3;state.gpr[1]=state.stack_low;state.eflags=0xad7;
        const uint8_t bytes[]={(uint8_t)opcodes[i],0x01};
        assert(run(bytes,2,0x9000)==0 && state.eflags==flags[i]);
        memcpy(&value,stack.write_base,4);
        assert(state.gpr[0]==(i&1?answers[i]:3) && value==(i&1?5:answers[i]));
        state.gpr[1]=state.stack_high-3;state.eflags=0xad7;uint32_t before=state.gpr[0];
        assert(run(bytes,2,0x9010)==-1 && state.gpr[0]==before && state.eflags==0xad7 && state.eip==0x9010);
    }
    /* Address and destination register may alias. */
    uint32_t value=0x12345678;memcpy(stack.write_base,&value,4);
    state.gpr[0]=state.stack_low;
    const uint8_t alias[]={0x33,0x00};
    assert(run(alias,2,0x9020)==0 && state.gpr[0]==(state.stack_low^value));
}
static void logical_test_tests(void)
{
    const uint32_t values[]={0,1,0x80000000,0x7fffffff,0xffffffff,0xaabbccdd};
    for(unsigned a=0;a<6;a++)for(unsigned b=0;b<6;b++)for(unsigned form=0;form<5;form++) {
        uint32_t left=values[a],right=values[b],result=left&right;
        state.gpr[0]=left;state.gpr[1]=right;state.gpr[2]=state.stack_low;
        memcpy(stack.write_base,&left,4);state.eflags=0xad7;
        unsigned flags=0x212;
        if(!result)flags|=0x40;
        if(result&0x80000000)flags|=0x80;
        unsigned parity=0;for(unsigned i=0;i<8;i++)parity^=(result>>i)&1;
        if(!parity)flags|=4;
        uint8_t bytes[8];size_t n=0;
        if(form<2){bytes[n++]=0x85;bytes[n++]=form?0x0a:0xc8;}
        else {
            bytes[n++]=form==2?0xa9:0xf7;
            if(form!=2)bytes[n++]=form==3?0xc0:0x02;
            for(unsigned i=0;i<4;i++)bytes[n++]=(uint8_t)(right>>(i*8));
        }
        assert(run(bytes,n,0x8000)==0 && state.eflags==flags);
        uint32_t after;memcpy(&after,stack.write_base,4);
        assert(state.gpr[0]==left && state.gpr[1]==right && after==left);
    }
    state.gpr[2]=state.stack_high-3;state.eflags=0xad7;
    const uint8_t bad[]={0x85,0x0a};
    assert(run(bad,2,0x8010)==-1 && state.eflags==0xad7 && state.eip==0x8010);
}
static void push_operand_tests(void)
{
    uint32_t slot=state.stack_high-8,value=0xaabbccdd;
    memcpy((void *)(uintptr_t)slot,&value,4);
    state.gpr[4]=slot;state.eflags=0xad7;
    const uint8_t from_esp[]={0xff,0x34,0x24};
    assert(run(from_esp,3,0x7000)==0 && state.gpr[4]==slot-4 && state.eip==0x7003 && state.eflags==0xad7);
    uint32_t pushed;memcpy(&pushed,(void *)(uintptr_t)(slot-4),4);assert(pushed==value);
    const uint8_t reg_esp[]={0xff,0xf4};state.gpr[4]=slot;
    assert(run(reg_esp,2,0x7010)==0);
    memcpy(&pushed,(void *)(uintptr_t)(slot-4),4);assert(pushed==slot);
    /* Source valid but destination underflows: do not commit ESP or memory. */
    state.gpr[4]=state.stack_low;value=123;memcpy(stack.write_base,&value,4);
    assert(run(from_esp,3,0x7020)==-1 && state.gpr[4]==state.stack_low && state.eflags==0xad7);
    memcpy(&pushed,stack.write_base,4);assert(pushed==123);
    /* Destination valid but source crosses the readable stack boundary. */
    state.gpr[4]=state.stack_high-2;
    assert(run(from_esp,3,0x7030)==-1 && state.gpr[4]==state.stack_high-2 && state.eip==0x7030);
    const uint8_t continuation[]={0xff,0xf0,0x5a};
    state.gpr[4]=slot;state.gpr[0]=42;
    assert(run(continuation,3,0x7040)==0 && state.gpr[2]==42 && state.gpr[4]==slot && state.eip==0x7043);
}
static void absolute_tests(void)
{
    uint32_t low=state.stack_low,high=state.stack_high;
    state.stack_low=state.stack_high=0;state.memory_count=1;
    for(unsigned write=0;write<2;write++)for(unsigned permission=0;permission<4;permission++)
    for(unsigned edge=0;edge<3;edge++) {
        uint32_t address=edge==0?low:edge==1?high-4:high-3;
        state.memory[0]=(PwX86Memory){low,high,permission};
        uint32_t original=0x11223344;memcpy(stack.write_base,&original,4);
        memcpy((uint8_t *)stack.write_base+stack.bytes-4,&original,4);
        state.gpr[0]=0xaabbccdd;state.eflags=0xad7;
        uint8_t instruction[]={write?0xa3:0xa1,(uint8_t)address,(uint8_t)(address>>8),(uint8_t)(address>>16),(uint8_t)(address>>24)};
        unsigned allowed=edge!=2 && (permission&(write?PW_X86_WRITE:PW_X86_READ));
        assert(run(instruction,5,0x6000)==(allowed?0:-1));
        assert(state.eflags==0xad7 && state.eip==(allowed?0x6005:0x6000));
        assert(state.gpr[0]==(allowed && !write?original:0xaabbccdd));
        uint32_t actual;memcpy(&actual,edge?(uint8_t *)stack.write_base+stack.bytes-4:stack.write_base,4);
        assert(actual==(allowed && write?0xaabbccdd:original));
    }

    /* Inline memory checks must reject every malformed registry state that
     * the generic helper rejects; a fast path is not a weaker contract. */
    state.memory[0]=(PwX86Memory){low,(uint64_t)high+0x100000000ull,PW_X86_READ};
    state.gpr[0]=0x11223344;state.eflags=0xad7;
    uint8_t malformed_high[]={0xa1,(uint8_t)low,(uint8_t)(low>>8),
        (uint8_t)(low>>16),(uint8_t)(low>>24)};
    assert(run(malformed_high,sizeof(malformed_high),0x6010)==-1 &&
           state.gpr[0]==0x11223344 && state.eip==0x6010 && state.eflags==0xad7);

    state.stack_low=low;state.stack_high=high;
    state.memory_count=PW_X86_MEMORY_REGIONS+1;
    state.gpr[0]=low;state.gpr[2]=0xaabbccdd;state.eflags=0xad7;
    const uint8_t corrupt_registry_stack_load[]={0x8b,0x10};
    assert(run(corrupt_registry_stack_load,sizeof(corrupt_registry_stack_load),0x6020)==-1 &&
           state.gpr[2]==0xaabbccdd && state.eip==0x6020 && state.eflags==0xad7);

    state.stack_low=state.stack_high=0;state.memory_count=1;
    state.memory[0]=(PwX86Memory){0,4,PW_X86_READ};
    state.gpr[0]=0x55667788;state.eflags=0xad7;
    const uint8_t null_load[]={0xa1,0,0,0,0};
    assert(run(null_load,sizeof(null_load),0x6030)==-1 &&
           state.gpr[0]==0x55667788 && state.eip==0x6030 && state.eflags==0xad7);

    state.stack_low=low;state.stack_high=high;state.memory_count=0;
}

static void optimization_safety_tests(void)
{
    uint8_t scratch[256];PwX86Block block;
    const uint8_t truncated_shift[]={0xc1,0xe0};
    assert(pw_x86_translate(truncated_shift,sizeof(truncated_shift),0x6000,
                            scratch,sizeof(scratch),&block)==PW_ERR_TRUNCATED);

    /* A faulting RMW observes flags produced by the preceding instruction.
     * Dead-flag elimination must materialize them before the memory guard. */
    const uint8_t fault_after_flags[]={
        0x05,1,0,0,0,       /* add eax, 1 */
        0x83,0x01,1         /* add dword [ecx], 1 (faults) */
    };
    state.gpr[0]=UINT32_MAX;state.gpr[1]=state.stack_high-3;state.eflags=0x202;
    assert(run(fault_after_flags,sizeof(fault_after_flags),0x6040)==-1 &&
           state.gpr[0]==0 && state.eip==0x6045 && state.eflags==0x257);

    /* Both static and dynamic zero-count shifts preserve the XOR result's
     * flags.  Treating a zero-count shift as an unconditional definition
     * would incorrectly eliminate that producer. */
    const uint8_t shift_cl_zero[]={0x31,0xc0,0xd3,0xe2,0x74,0xfe};
    state.gpr[0]=1;state.gpr[1]=0;state.gpr[2]=0x12345678;state.eflags=0x202;
    assert(run(shift_cl_zero,sizeof(shift_cl_zero),0x6050)==0 &&
           state.eip==0x6054 && state.gpr[2]==0x12345678 && state.eflags==0x246);

    const uint8_t shift_imm_zero[]={0x31,0xc0,0xc1,0xe2,0,0x74,0xfe};
    state.gpr[0]=1;state.gpr[2]=0x12345678;state.eflags=0x202;
    assert(run(shift_imm_zero,sizeof(shift_imm_zero),0x6060)==0 &&
           state.eip==0x6065 && state.gpr[2]==0x12345678 && state.eflags==0x246);

    /* Multi-bit shifts leave OF undefined; this runtime preserves its prior
     * deterministic value.  DFE must therefore keep an earlier OF producer. */
    const uint8_t shift_imm_two[]={0x6b,0xc0,2,0xc1,0xe2,2,0x70,0xfe};
    state.gpr[0]=0x40000000;state.gpr[2]=1;state.eflags=0x202;
    assert(run(shift_imm_two,sizeof(shift_imm_two),0x6070)==0 &&
           state.eip==0x6076 && state.gpr[0]==0x80000000 &&
           state.gpr[2]==4 && state.eflags==0xa02);
}
static void immediate_tests(void)
{
    state.gpr[2]=0xaabbccdd;state.eflags=0xad7;
    const uint8_t mov_dx_word[]={0x66,0xc7,0xc2,0x32,0x54};
    assert(run(mov_dx_word,sizeof(mov_dx_word),0x7ef)==PW_OK &&
           state.gpr[2]==0xaabb5432 && state.eflags==0xad7);
    state.gpr[2]=state.stack_low+24;*(uint32_t *)((uint8_t *)stack.write_base+24)=0xaabbccdd;
    const uint8_t mov_memory_word[]={0x66,0xc7,0x02,0x34,0x12};
    assert(run(mov_memory_word,sizeof(mov_memory_word),0x7f4)==PW_OK &&
           *(uint32_t *)((uint8_t *)stack.write_base+24)==0xaabb1234);
    const uint32_t inputs[]={0,1,5,0x7fff,0x8000,0x7fffffff,0x80000000,0xffffffff};
    for(unsigned width=0;width<2;width++)for(unsigned op=0;op<8;op++)
    for(unsigned cf=0;cf<2;cf++)for(unsigned sample=0;sample<8;sample++)
    for(unsigned memory=0;memory<2;memory++)for(unsigned encoding=0;encoding<3;encoding++) {
        if(memory && encoding==2)continue;
        uint32_t mask=width?0xffff:UINT32_MAX,sign=width?0x8000:0x80000000;
        uint32_t a=inputs[sample]&mask,b=3,result;
        unsigned carry=(op==2 || op==3)?cf:0,logical=op==1 || op==4 || op==6;
        uint64_t wide;
        if(op==0 || op==2){wide=(uint64_t)a+b+carry;result=(uint32_t)wide&mask;}
        else if(op==1){wide=0;result=a|b;}
        else if(op==4){wide=0;result=a&b;}
        else if(op==6){wide=0;result=a^b;}
        else {wide=(uint64_t)b+carry;result=(a-(uint32_t)wide)&mask;}
        unsigned flags=0x202;
        if(logical)flags|=0x10; /* undefined AF retained */
        else {
            unsigned sub=op==3 || op==5 || op==7;
            if(sub?a<wide:wide>mask)flags|=1;
            if((a^b^result)&16)flags|=16;
            if((sub?((a^b)&(a^result)):(~(a^b)&(a^result)))&sign)flags|=0x800;
        }
        if(!result)flags|=0x40;
        if(result&sign)flags|=0x80;
        unsigned parity=0;for(unsigned bit=0;bit<8;bit++)parity^=(result>>bit)&1;
        if(!parity)flags|=4;
        uint32_t initial=inputs[sample];state.gpr[0]=initial;state.gpr[1]=state.stack_low;
        memcpy(stack.write_base,&initial,4);state.eflags=0x212|cf;
        uint8_t bytes[8];size_t n=0;if(width)bytes[n++]=0x66;
        if(encoding==2)bytes[n++]=(uint8_t)(5+op*8);
        else {bytes[n++]=encoding?0x81:0x83;bytes[n++]=(uint8_t)((memory?1:0xc0)|(op<<3));}
        bytes[n++]=3;
        if(encoding){bytes[n++]=0;if(!width){bytes[n++]=0;bytes[n++]=0;}}
        assert(run(bytes,n,0x5000)==0 && state.eflags==flags);
        uint32_t actual=state.gpr[0];if(memory)memcpy(&actual,stack.write_base,4);
        uint32_t expected=op==7?initial:(initial&~mask)|result;
        assert(actual==expected);
    }
    /* RMW requires both permissions, unlike CMP's read-only access. */
    uint32_t low=state.stack_low,high=state.stack_high;
    state.stack_low=state.stack_high=0;state.memory_count=1;
    state.gpr[0]=low;
    const uint8_t update[]={0x83,0x08,1};
    for(unsigned permission=1;permission<=2;permission++) {
        state.memory[0]=(PwX86Memory){low,high,permission};state.eflags=0x246;
        uint32_t before;memcpy(&before,stack.write_base,4);
        assert(run(update,3,0x5100)==-1 && state.eflags==0x246 && state.eip==0x5100);
        uint32_t after;memcpy(&after,stack.write_base,4);assert(after==before);
    }
    state.stack_low=low;state.stack_high=high;state.memory_count=0;
}

static void lock_prefix_tests(void)
{
    const uint32_t low=state.stack_low;
    const uint32_t high=state.stack_high;
    const uint8_t lock_add[]={0xf0,0x83,0x00,0x01};   /* lock add [eax],1 */
    const uint8_t plain_add[]={0x83,0x00,0x01};
    uint32_t flags=0;

    state.memory_count=0;
    state.gpr[0]=low;
    state.eflags=0x202;
    uint32_t seed=5;memcpy((void *)(uintptr_t)low,&seed,4);
    assert(run(lock_add,sizeof(lock_add),0x7000)==0);
    uint32_t memory=0;memcpy(&memory,(void *)(uintptr_t)low,4);
    flags=state.eflags;
    assert(memory==6 && (flags&1)==0 && (flags&0x40)==0);
    /* The same instruction without LOCK produces the same result and the
     * same flags: the prefix asks for atomicity, it does not change
     * architectural semantics. */
    state.eflags=0x202;
    memcpy((void *)(uintptr_t)low,&seed,4);
    assert(run(plain_add,sizeof(plain_add),0x7010)==0);
    uint32_t plain_memory=0;memcpy(&plain_memory,(void *)(uintptr_t)low,4);
    assert(plain_memory==memory && state.eflags==flags);

    /* Only legal LOCK forms are accepted. A register destination or a pure
     * compare is not one, and neither is a form this translator does not
     * handle yet: those must stay refused rather than silently unlocked. */
    uint8_t scratch[4096];PwX86Block block;
    const uint8_t lock_register[]={0xf0,0x83,0xc0,0x01};
    const uint8_t lock_compare[]={0xf0,0x83,0x38,0x01};
    const uint8_t lock_inc[]={0xf0,0xff,0x00};
    const uint8_t lock_unterminated[]={0xf0,0x83};
    assert(pw_x86_translate(lock_register,sizeof(lock_register),0,scratch,sizeof(scratch),&block)==PW_ERR_UNSUPPORTED);
    assert(pw_x86_translate(lock_compare,sizeof(lock_compare),0,scratch,sizeof(scratch),&block)==PW_ERR_UNSUPPORTED);
    assert(pw_x86_translate(lock_inc,sizeof(lock_inc),0,scratch,sizeof(scratch),&block)==PW_ERR_UNSUPPORTED);
    assert(pw_x86_translate(lock_unterminated,sizeof(lock_unterminated),0,scratch,sizeof(scratch),&block)==PW_ERR_TRUNCATED);
    /* A locked read-modify-write still requires write permission. */
    state.stack_low=state.stack_high=0;
    state.memory_count=1;
    state.memory[0]=(PwX86Memory){low,high,PW_X86_READ};
    state.gpr[0]=low;
    uint32_t before;memcpy(&before,(void *)(uintptr_t)low,4);
    assert(run(lock_add,sizeof(lock_add),0x7020)==-1);
    uint32_t after;memcpy(&after,(void *)(uintptr_t)low,4);
    assert(after==before);
    state.memory_count=0;
    state.stack_low=low;state.stack_high=high;

    /*
     * LOCK CMPXCHG to memory, the compare-and-swap Wine's own atomics use: the
     * word is compared with the accumulator and the source register is stored
     * into it when they are equal. Both outcomes are checked, because they are
     * different instructions' worth of behaviour: on equality the accumulator
     * is untouched and ZF is set, and on inequality the *old* word goes into
     * the accumulator and the memory keeps its value.
     */
    {
        const uint8_t lock_cmpxchg[]={0xf0,0x0f,0xb1,0x11}; /* [ecx], edx */
        const uint8_t plain_cmpxchg[]={0x0f,0xb1,0x11};
        const uint8_t cmpxchg_register[]={0xf0,0x0f,0xb1,0xd0};
        const uint8_t cmpxchg_byte[]={0xf0,0x0f,0xb0,0x10};
        uint32_t word=0;

        state.memory_count=0;
        state.stack_low=low;state.stack_high=high;
        /* The address lives in ECX, because EAX is the accumulator this
         * instruction compares with the word. */
        state.gpr[1]=low;
        state.gpr[2]=0xaabbccddu;
        state.eflags=0x202;
        word=0x11223344u;memcpy((void *)(uintptr_t)low,&word,4);
        /* Equal: the source is stored and the accumulator does not move. */
        state.gpr[0]=0x11223344u;
        state.gpr[2]=0xaabbccddu;
        assert(run(lock_cmpxchg,sizeof(lock_cmpxchg),0x7100)==0);
        memcpy(&word,(void *)(uintptr_t)low,4);
        assert(word==0xaabbccddu);
        assert(state.gpr[0]==0x11223344u);
        assert((state.eflags&0x40)!=0);                 /* ZF: they were equal */
        /* The same instruction without LOCK: atomicity is the prefix's only
         * promise, so the architectural result has to be identical. */
        state.gpr[0]=0x11223344u;
        state.gpr[2]=0x55667788u;
        word=0x11223344u;memcpy((void *)(uintptr_t)low,&word,4);
        state.eflags=0x202;
        assert(run(plain_cmpxchg,sizeof(plain_cmpxchg),0x7110)==0);
        memcpy(&word,(void *)(uintptr_t)low,4);
        assert(word==0x55667788u);
        assert(state.gpr[0]==0x11223344u && (state.eflags&0x40)!=0);
        /* Not equal: the memory keeps its word and the accumulator takes it. */
        state.gpr[0]=0x55667788u;
        state.gpr[2]=0x11111111u;
        word=0x11223344u;memcpy((void *)(uintptr_t)low,&word,4);
        state.eflags=0x202;
        assert(run(lock_cmpxchg,sizeof(lock_cmpxchg),0x7120)==0);
        memcpy(&word,(void *)(uintptr_t)low,4);
        assert(word==0x11223344u);
        assert(state.gpr[0]==0x11223344u);
        assert((state.eflags&0x40)==0);                 /* ZF: they differed */
        /* The flags are those of the accumulator minus the word, which is
         * what a real i386 reports here: the accumulator is the larger value,
         * so nothing is borrowed and CF stays clear (the differential test
         * against native code is what settled the direction). */
        assert((state.eflags&0x1)==0);
        /* The register form has no atom to lock and the byte and 16-bit forms
         * are not implemented: all of them stay refused. */
        uint8_t scratch2[4096];PwX86Block block2;
        assert(pw_x86_translate(cmpxchg_register,sizeof(cmpxchg_register),0,
                               scratch2,sizeof(scratch2),&block2)==PW_ERR_UNSUPPORTED);
        assert(pw_x86_translate(cmpxchg_byte,sizeof(cmpxchg_byte),0,
                               scratch2,sizeof(scratch2),&block2)==PW_ERR_UNSUPPORTED);
        /*
         * XCHG: the memory form is what Wine's heap code uses to take an entry
         * off a free list, with or without the redundant LOCK prefix, and both
         * are translated. The register form has no memory operand to exchange
         * and the byte and 16-bit forms are not implemented, so all three stay
         * refused rather than half-translated.
         */
        {
            const uint8_t xchg_memory[]={0x87,0x11};      /* xchg [ecx], edx */
            const uint8_t xchg_locked[]={0xf0,0x87,0x11}; /* lock xchg [ecx],edx */
            const uint8_t xchg_register[]={0x87,0xd0};    /* xchg eax, edx */
            const uint8_t xchg_byte[]={0x86,0x10};        /* xchg [eax], dl */
            const uint8_t xchg_word[]={0x66,0x87,0x11};   /* xchg [ecx], dx */

            assert(pw_x86_translate(xchg_memory,sizeof(xchg_memory),0,
                                    scratch2,sizeof(scratch2),&block2)==PW_OK);
            assert(pw_x86_translate(xchg_locked,sizeof(xchg_locked),0,
                                    scratch2,sizeof(scratch2),&block2)==PW_OK);
            assert(pw_x86_translate(xchg_register,sizeof(xchg_register),0,
                                    scratch2,sizeof(scratch2),&block2)==PW_ERR_UNSUPPORTED);
            assert(pw_x86_translate(xchg_byte,sizeof(xchg_byte),0,
                                    scratch2,sizeof(scratch2),&block2)==PW_ERR_UNSUPPORTED);
            assert(pw_x86_translate(xchg_word,sizeof(xchg_word),0,
                                    scratch2,sizeof(scratch2),&block2)==PW_ERR_UNSUPPORTED);
        }
        /* A locked compare-and-swap still requires write permission. */
        state.stack_low=state.stack_high=0;
        state.memory_count=1;
        state.memory[0]=(PwX86Memory){low,high,PW_X86_READ};
        state.gpr[1]=low;
        state.gpr[0]=0x11223344u;
        state.gpr[2]=0x11111111u;
        word=0x11223344u;memcpy((void *)(uintptr_t)low,&word,4);
        assert(run(lock_cmpxchg,sizeof(lock_cmpxchg),0x7130)==-1);
        memcpy(&word,(void *)(uintptr_t)low,4);
        assert(word==0x11223344u);
        state.memory_count=0;
        state.stack_low=low;
        state.stack_high=high;
    }
}
/* Fills one guest XMM register from a 4-dword pattern. */
static void set_xmm(unsigned reg, uint32_t d0, uint32_t d1, uint32_t d2,
                    uint32_t d3)
{
    uint32_t words[4];

    words[0] = d0; words[1] = d1; words[2] = d2; words[3] = d3;
    memcpy(state.fp.xmm[reg], words, sizeof(words));
}

static int xmm_is(unsigned reg, uint32_t d0, uint32_t d1, uint32_t d2,
                  uint32_t d3)
{
    uint32_t words[4];

    memcpy(words, state.fp.xmm[reg], sizeof(words));
    return words[0] == d0 && words[1] == d1 && words[2] == d2 && words[3] == d3;
}

/*
 * The SSE slice and the bit-scan family. Guest XMM state is ordinary guest
 * memory, so the tests can read it directly; each case is also run through
 * every residency/lazy-flag combination because that is what caught the
 * earlier cmov defect.
 */
static void sse_and_scan_tests(void)
{
    const uint32_t low=state.stack_low;
    const uint8_t movd_load[]={0x66,0x0f,0x6e,0xc0};      /* movd xmm0,eax */
    const uint8_t movd_store[]={0x66,0x0f,0x7e,0xc0};     /* movd eax,xmm0 */
    const uint8_t movdqu_reg[]={0xf3,0x0f,0x6f,0xd1};     /* movdqu xmm2,xmm1 */
    const uint8_t movdqu_mem[]={0xf3,0x0f,0x6f,0x00};     /* movdqu xmm0,[eax] */
    const uint8_t punpckldq[]={0x66,0x0f,0x62,0xc0};      /* punpckldq xmm0,xmm0 */
    const uint8_t punpcklqdq[]={0x66,0x0f,0x6c,0xd0};     /* punpcklqdq xmm2,xmm0 */
    const uint8_t pxor[]={0x66,0x0f,0xef,0xc1};           /* pxor xmm0,xmm1 */
    const uint8_t movups_store[]={0x0f,0x11,0x10};        /* movups [eax],xmm2 */
    const uint8_t movq_store[]={0x66,0x0f,0xd6,0x00};     /* movq [eax],xmm0 */
    const uint8_t movss_load[]={0xf3,0x0f,0x10,0x08};     /* movss xmm1,[eax] */
    const uint8_t movsd_load[]={0xf2,0x0f,0x10,0x08};     /* movsd xmm1,[eax] */
    const uint8_t movq_load[]={0xf3,0x0f,0x7e,0x00};      /* movq xmm0,[eax] */
    const uint8_t pshufd[]={0x66,0x0f,0x70,0xc0,0x00};
    const uint8_t pextrw[]={0x66,0x0f,0xc5,0xc8,0x01};
    const uint8_t pinsrw[]={0x66,0x0f,0xc4,0xc1,0x02};
    const uint8_t bsr[]={0x0f,0xbd,0xca};
    const uint8_t bsf[]={0x0f,0xbc,0xca};
    const uint8_t bsr16[]={0x66,0x0f,0xbd,0xc2};
    const uint8_t tzcnt[]={0xf3,0x0f,0xbc,0xca};          /* tzcnt ecx,edx */
    const uint8_t lzcnt[]={0xf3,0x0f,0xbd,0xca};          /* lzcnt ecx,edx */
    const uint8_t shr16[]={0x66,0xd1,0xea};               /* shr dx,1 */
    const uint8_t shl16_imm[]={0x66,0xc1,0xe0,0x04};      /* shl ax,4 */
    const uint8_t shr16_cl[]={0x66,0xd3,0xe8};            /* shr ax,cl */
    const uint8_t nop16[]={0x66,0x90};                    /* xchg ax,ax */
    const uint8_t nop_multibyte[]={0x0f,0x1f,0x44,0x00,0x00};
    const uint8_t lea_cs[]={0x2e,0x8d,0x74,0x26,0x00};   /* padding form */

    state.memory_count=0;
    memset(state.fp.xmm,0,sizeof(state.fp.xmm));
    /* movd xmm0, eax zeroes everything above the low dword. */
    state.gpr[0]=0x11223344;state.fp.xmm[0][4]=0xaa;state.eflags=0xad7;
    assert(run(movd_load,sizeof(movd_load),0x8000)==0);
    assert(xmm_is(0,0x11223344,0,0,0) && state.eflags==0xad7);
    set_xmm(0,0x55667788,0x99aabbcc,0xddeeff00,0x12345678);
    assert(run(movd_store,sizeof(movd_store),0x8010)==0);
    assert(state.gpr[0]==0x55667788);
    /* Register-to-register copies and the lane shuffles. */
    set_xmm(1,0x11111111,0x22222222,0x33333333,0x44444444);
    memset(state.fp.xmm[2],0,sizeof(state.fp.xmm[2]));
    assert(run(movdqu_reg,sizeof(movdqu_reg),0x8020)==0);
    assert(xmm_is(2,0x11111111,0x22222222,0x33333333,0x44444444));
    set_xmm(0,0xaaaaaaaa,0xbbbbbbbb,0xcccccccc,0xdddddddd);
    assert(run(punpckldq,sizeof(punpckldq),0x8030)==0);
    assert(xmm_is(0,0xaaaaaaaa,0xaaaaaaaa,0xbbbbbbbb,0xbbbbbbbb));
    set_xmm(2,0x01010101,0x02020202,0x03030303,0x04040404);
    set_xmm(0,0x05050505,0x06060606,0x07070707,0x08080808);
    assert(run(punpcklqdq,sizeof(punpcklqdq),0x8040)==0);
    assert(xmm_is(2,0x01010101,0x02020202,0x05050505,0x06060606));
    set_xmm(0,0xf0f0f0f0,0x0f0f0f0f,0xffffffff,0x00000000);
    set_xmm(1,0x0000ffff,0xffff0000,0x12345678,0x87654321);
    assert(run(pxor,sizeof(pxor),0x8050)==0);
    assert(xmm_is(0,0xf0f00f0f,0xf0f00f0f,0xedcba987,0x87654321));
    /* 16-byte and 8-byte stores into guest memory, and the scalar loads. */
    memset((void *)(uintptr_t)low,0,32);
    state.gpr[0]=low;set_xmm(2,1,2,3,4);
    assert(run(movups_store,sizeof(movups_store),0x8060)==0);
    uint32_t words[4];memcpy(words,(void *)(uintptr_t)low,sizeof(words));
    assert(words[0]==1 && words[1]==2 && words[2]==3 && words[3]==4);
    set_xmm(0,5,6,7,8);
    state.gpr[0]=low+16;
    assert(run(movq_store,sizeof(movq_store),0x8070)==0);
    memcpy(words,(void *)(uintptr_t)(low+16),sizeof(words));
    assert(words[0]==5 && words[1]==6 && words[2]==0 && words[3]==0);
    memcpy(words,(void *)(uintptr_t)low,sizeof(words));
    state.gpr[0]=low;set_xmm(1,0x11111111,0x22222222,0x33333333,0x44444444);
    assert(run(movdqu_mem,sizeof(movdqu_mem),0x8080)==0);
    assert(xmm_is(0,1,2,3,4));
    memset(state.fp.xmm[1],0x5a,sizeof(state.fp.xmm[1]));
    assert(run(movss_load,sizeof(movss_load),0x8090)==0);
    assert(xmm_is(1,1,0,0,0));
    memset(state.fp.xmm[1],0x5a,sizeof(state.fp.xmm[1]));
    assert(run(movsd_load,sizeof(movsd_load),0x80a0)==0);
    assert(xmm_is(1,1,2,0,0));
    memset(state.fp.xmm[0],0x5a,sizeof(state.fp.xmm[0]));
    assert(run(movq_load,sizeof(movq_load),0x80b0)==0);
    assert(xmm_is(0,1,2,0,0));
    /* Broadcast, extract and insert. */
    set_xmm(0,0xdeadbeef,0xfeedface,0x0badf00d,0xcafebabe);
    assert(run(pshufd,sizeof(pshufd),0x80c0)==0);
    assert(xmm_is(0,0xdeadbeef,0xdeadbeef,0xdeadbeef,0xdeadbeef));
    state.gpr[0]=0xffffffff;
    assert(run(pextrw,sizeof(pextrw),0x80d0)==0);
    /* ModRM c8 selects ECX as the destination; word 1 of 0xdeadbeef is dead. */
    assert(state.gpr[1]==0x0000dead);
    state.gpr[1]=0x1234;
    assert(run(pinsrw,sizeof(pinsrw),0x80e0)==0);
    assert(xmm_is(0,0xdeadbeef,0xdead1234,0xdeadbeef,0xdeadbeef));
    /* BSF/BSR: index in the destination, ZF from the source, and a zero
     * source leaves the destination unchanged. */
    state.gpr[2]=0x1000;state.gpr[1]=0xffffffff;state.eflags=0x202;
    assert(run(bsr,sizeof(bsr),0x8100)==0);
    assert(state.gpr[1]==12 && (state.eflags&0x40)==0);
    /* 0xdeadbeef has bit 0 set, so BSF reports index 0. */
    state.gpr[2]=0xdeadbeef;state.gpr[1]=0xffffffff;state.eflags=0x202;
    assert(run(bsf,sizeof(bsf),0x8110)==0);
    assert(state.gpr[1]==0 && (state.eflags&0x40)==0);
    state.gpr[2]=0;state.gpr[1]=0x55;state.eflags=0x202;
    assert(run(bsr,sizeof(bsr),0x8120)==0);
    assert(state.gpr[1]==0x55 && (state.eflags&0x40)!=0);
    state.gpr[2]=0x8000;state.gpr[0]=0xffffffff;state.eflags=0x202;
    assert(run(bsr16,sizeof(bsr16),0x8130)==0);
    assert(state.gpr[0]==0xffff000f);
    /* TZCNT/LZCNT: a count rather than an index, defined for a zero source
     * (32, with CF set), and ZF reports a zero count, not a zero source. */
    state.gpr[2]=0x1000;state.gpr[1]=0xffffffff;state.eflags=0x202|0x41;
    assert(run(tzcnt,sizeof(tzcnt),0x8132)==0);
    assert(state.gpr[1]==12 && (state.eflags&0x41)==0);
    assert(run(lzcnt,sizeof(lzcnt),0x8134)==0);
    assert(state.gpr[1]==19 && (state.eflags&0x41)==0);
    state.gpr[2]=0;state.gpr[1]=0x55;state.eflags=0x202;
    assert(run(tzcnt,sizeof(tzcnt),0x8136)==0);
    assert(state.gpr[1]==32 && (state.eflags&0x41)==0x01);
    assert(run(lzcnt,sizeof(lzcnt),0x8138)==0);
    assert(state.gpr[1]==32 && (state.eflags&0x41)==0x01);
    state.gpr[2]=0x80000001;state.eflags=0x202;
    assert(run(tzcnt,sizeof(tzcnt),0x813a)==0);
    assert(state.gpr[1]==0 && (state.eflags&0x41)==0x40);
    assert(run(lzcnt,sizeof(lzcnt),0x813c)==0);
    assert(state.gpr[1]==0 && (state.eflags&0x41)==0x40);
    /* The 16-bit shift group: only the low word changes, the count is masked
     * to five bits, and a masked-zero count preserves every flag. */
    state.gpr[2]=0x00070008;state.eflags=0x202|1;
    assert(run(shr16,sizeof(shr16),0x8140)==0);
    assert(state.gpr[2]==0x00070004);
    assert((state.eflags&1)==0 && (state.eflags&0x40)==0);
    state.gpr[0]=0x0000ffff;state.eflags=0x202;
    assert(run(shl16_imm,sizeof(shl16_imm),0x8150)==0);
    assert(state.gpr[0]==0x0000fff0 && (state.eflags&0x40)==0);
    /* A count of 16 or more empties the word, as on the host; 32 is zero. */
    for(unsigned count=16;count<=32;count+=16) {
        uint32_t expected=0x00018001;unsigned long flags;
        __asm__ volatile("shrw %%cl,%w0; pushfq; popq %1":"+a"(expected),"=r"(flags):"c"(count):"cc");
        state.gpr[0]=0x00018001;state.gpr[1]=count;state.eflags=0xad7;
        assert(run(shr16_cl,sizeof(shr16_cl),0x8160)==0);
        const unsigned mask=count==32?0:0xc5;
        assert(state.gpr[0]==expected && state.eflags==((0xad7&~mask)|((unsigned)flags&mask)));
    }
    assert(state.gpr[0]==0x00018001);
    state.gpr[0]=0x00008000;state.gpr[1]=1;state.eflags=0x202;
    assert(run(shr16_cl,sizeof(shr16_cl),0x8170)==0);
    assert(state.gpr[0]==0x00004000 && (state.eflags&1)==0);
    /* Padding and prefix forms: a 16-bit NOP, a multi-byte NOP, and a
     * segment override on LEA (which never accesses memory). None of them
     * may change a register or a flag. */
    state.gpr[6]=0x12345678;state.gpr[0]=0xabcdef01;state.eflags=0xad7;
    assert(run(nop16,sizeof(nop16),0x8180)==0);
    assert(state.gpr[0]==0xabcdef01 && state.eflags==0xad7 && state.eip==0x8182);
    assert(run(nop_multibyte,sizeof(nop_multibyte),0x8190)==0);
    assert(state.gpr[0]==0xabcdef01 && state.eip==0x8195);
    state.gpr[6]=0x00001000;state.eflags=0xad7;
    assert(run(lea_cs,sizeof(lea_cs),0x81a0)==0);
    assert(state.gpr[6]==0x00001000 && state.eflags==0xad7 && state.eip==0x81a5);
    /* Every one of them must behave identically in all engine modes. */
    /*
     * The idiom Wine's RtlFormatCurrentUserKeyPath uses to widen a
     * UNICODE_STRING's length and maximum length at once: load the dword that
     * holds both 16-bit halves with movd from guest memory, add the prefix
     * length in both lanes with paddw, and store it back with movd. The
     * constant vector comes from guest memory too, so the whole sequence is
     * exercised the way the real code uses it.
     */
    {
        uint8_t movd_mem_load[]={0x66,0x0f,0x6e,0x03};      /* movd xmm0,[ebx] */
        uint8_t movd_mem_store[]={0x66,0x0f,0x7e,0x03};     /* movd [ebx],xmm0 */
        uint8_t movd_abs_load[]={0x66,0x0f,0x6e,0x0d,0,0,0,0};
        const uint8_t paddw_reg[]={0x66,0x0f,0xfd,0xc1};    /* paddw xmm0,xmm1 */
        const uint8_t pcmpeqb_reg[]={0x66,0x0f,0x74,0xc1};  /* pcmpeqb xmm0,xmm1 */
        const uint8_t pmovmskb_reg[]={0x66,0x0f,0xd7,0xc1}; /* pmovmskb eax,xmm1 */
        const uint32_t length_dword=0x00000046u;            /* Length=0x46 */
        const uint32_t prefix=0x001e001eu;                  /* 30 in both halves */
        const unsigned char mask_bytes[4]={0x00,0x80,0x00,0xff};
        const uint32_t constant_address=low+0x110u;
        uint32_t mask=0, stored=0;

        memcpy(movd_abs_load+4,&constant_address,4u);
        memcpy(&mask,mask_bytes,sizeof(mask));
        memcpy((void *)(uintptr_t)(low+0x100),&length_dword,4u);
        memcpy((void *)(uintptr_t)(low+0x110),&prefix,4u);
        memcpy((void *)(uintptr_t)(low+0x120),&mask,4u);
        memset(state.fp.xmm,0,sizeof(state.fp.xmm));
        state.gpr[3]=low+0x100;                             /* ebx */
        assert(run(movd_mem_load,sizeof(movd_mem_load),0x81b0)==0);
        assert(xmm_is(0,0x00000046u,0,0,0));
        /* The constant comes from guest memory, not from a host symbol. */
        assert(run(movd_abs_load,sizeof(movd_abs_load),0x81c0)==0);
        assert(xmm_is(1,0x001e001e,0,0,0));
        assert(run(paddw_reg,sizeof(paddw_reg),0x81d0)==0);
        assert(xmm_is(0,0x001e0064u,0,0,0));
        state.gpr[3]=low+0x100;
        assert(run(movd_mem_store,sizeof(movd_mem_store),0x81e0)==0);
        memcpy(&stored,(void *)(uintptr_t)(low+0x100),4u);
        assert(stored==0x001e0064u);
        /* pcmpeqb produces all-ones bytes where the lanes are equal, and
         * pmovmskb reports the sign bit of each of the 16 lanes. */
        set_xmm(0,mask,0,0,0);
        set_xmm(1,mask,0,0,0);
        assert(run(pcmpeqb_reg,sizeof(pcmpeqb_reg),0x81f0)==0);
        assert(xmm_is(0,0xffffffffu,0xffffffffu,0xffffffffu,0xffffffffu));
        set_xmm(1,mask,0,0,0);
        assert(run(pmovmskb_reg,sizeof(pmovmskb_reg),0x8200)==0);
        assert(state.gpr[0]==0x0000000au);
    }
    for(unsigned residency=0;residency<2;residency++)
        for(unsigned lazy=0;lazy<2;lazy++) {
            memset(state.fp.xmm,0,sizeof(state.fp.xmm));
            state.gpr[0]=0x11223344;state.eflags=0x202;
            assert(run_mode(movd_load,sizeof(movd_load),0x8200,residency,lazy)==0);
            assert(xmm_is(0,0x11223344,0,0,0));
            set_xmm(0,0xaaaa0001,0xaaaa0002,0xaaaa0003,0xaaaa0004);
            assert(run_mode(punpckldq,sizeof(punpckldq),0x8210,residency,lazy)==0);
            assert(xmm_is(0,0xaaaa0001,0xaaaa0001,0xaaaa0002,0xaaaa0002));
            state.gpr[2]=0x40;state.gpr[1]=0;
            assert(run_mode(bsr,sizeof(bsr),0x8220,residency,lazy)==0);
            assert(state.gpr[1]==6 && (state.eflags&0x40)==0);
            state.gpr[2]=0;state.gpr[1]=0;
            assert(run_mode(tzcnt,sizeof(tzcnt),0x8224,residency,lazy)==0);
            assert(state.gpr[1]==32 && (state.eflags&0x41)==0x01);
            /* The packed-integer forms write no flags at all: whatever was
             * live before them must survive every mode combination. */
            {
                const uint8_t paddw_mode[]={0x66,0x0f,0xfd,0xc1};
                const uint8_t pcmpeqb_mode[]={0x66,0x0f,0x74,0xc1};
                const uint8_t pmovmskb_mode[]={0x66,0x0f,0xd7,0xc1};

                set_xmm(0,0x00010002,0,0,0);
                set_xmm(1,0x0000ffff,0,0,0);
                state.eflags=0xad7;
                assert(run_mode(paddw_mode,sizeof(paddw_mode),0x8230,
                                residency,lazy)==0);
                assert(xmm_is(0,0x00010001u,0,0,0) && state.eflags==0xad7);
                set_xmm(0,0,0,0,0);
                set_xmm(1,0,0,0,0);
                state.eflags=0xad7;
                assert(run_mode(pcmpeqb_mode,sizeof(pcmpeqb_mode),0x8240,
                                residency,lazy)==0);
                assert(xmm_is(0,0xffffffffu,0xffffffffu,0xffffffffu,
                              0xffffffffu) && state.eflags==0xad7);
                /* byte 0 and byte 8 have their sign bit set, so the mask is
                 * bit 0 and bit 8. */
                set_xmm(1,0x00000080u,0,0x00000080u,0);
                state.eflags=0xad7;
                assert(run_mode(pmovmskb_mode,sizeof(pmovmskb_mode),0x8250,
                                residency,lazy)==0);
                assert(state.gpr[0]==0x0101u && state.eflags==0xad7);
            }
        }
    /* MMX encodings of the same opcodes, the merging register forms and an
     * out-of-region 16-byte store all stay refused. */
    uint8_t scratch[4096];PwX86Block block;
    const uint8_t mmx_movq[]={0x0f,0x6f,0xc1};
    const uint8_t movq_reg[]={0xf3,0x0f,0x7e,0xc1};
    const uint8_t movss_reg[]={0xf3,0x0f,0x10,0xc1};
    const uint8_t movups_oob[]={0x0f,0x11,0x00};
    const uint8_t lea_cs_reg[]={0x2e,0x8d,0xc0};
    const uint8_t nop_bad[]={0x0f,0x1f,0xc8};
    /* The packed moves that require 16-byte alignment (movaps 0f 28/29,
     * movdqa 66 0f 6f/7f): an aligned memory operand moves the 16 bytes in
     * every mode, and a misaligned one is the guest's access violation at
     * 0xffffffff with memory, XMM and the instruction pointer untouched. */
    const uint8_t movaps_reg[]={0x0f,0x28,0xc3};
    assert(pw_x86_translate(movaps_reg,sizeof(movaps_reg),0,scratch,sizeof(scratch),&block)==PW_OK);
    {
        const uint8_t aligned_moves[][4]={{0x0f,0x28,0x10},{0x0f,0x29,0x10},
                                          {0x66,0x0f,0x6f,0x10},{0x66,0x0f,0x7f,0x10}};
        const size_t lengths[]={3,3,4,4};
        uint8_t pattern[16],before[32];
        for(unsigned i=0;i<16;i++)pattern[i]=(uint8_t)(0xa0+i);
        for(unsigned form=0;form<4;form++)for(unsigned mode=0;mode<4;mode++)
        for(unsigned misaligned=0;misaligned<2;misaligned++) {
            const int store=form==1||form==3;
            const uint32_t base=(state.stack_high-64)&~15u,address=base+(misaligned?4:0);
            uint8_t *memory=(uint8_t *)(uintptr_t)base;
            memset(memory,0x11,32);
            if(!store)memcpy(memory+(misaligned?4:0),pattern,16);
            memset(state.fp.xmm,0x22,sizeof(state.fp.xmm));
            if(store)memcpy(state.fp.xmm[2],pattern,16);
            memcpy(before,memory,32);
            state.gpr[0]=address;state.eflags=0xad7;state.fault_address=0;
            const int status=run_mode(aligned_moves[form],lengths[form],0xd600,mode&1,mode>>1);
            if(misaligned) {
                assert(status==-1 && state.eip==0xd600 && state.fault_address==0xffffffffu);
                assert(!memcmp(memory,before,32) && state.eflags==0xad7);
                uint8_t fill[16];memset(fill,0x22,16);
                if(!store)assert(!memcmp(state.fp.xmm[2],fill,16));
            } else {
                assert(status==0 && state.eflags==0xad7);
                assert(!memcmp(store?memory:state.fp.xmm[2],pattern,16));
                if(store)assert(memory[16]==0x11);
            }
        }
    }
    assert(pw_x86_translate(lea_cs_reg,sizeof(lea_cs_reg),0,scratch,sizeof(scratch),&block)==PW_ERR_UNSUPPORTED);
    assert(pw_x86_translate(nop_bad,sizeof(nop_bad),0,scratch,sizeof(scratch),&block)==PW_ERR_UNSUPPORTED);
    assert(pw_x86_translate(mmx_movq,sizeof(mmx_movq),0,scratch,sizeof(scratch),&block)==PW_ERR_UNSUPPORTED);
    assert(pw_x86_translate(movq_reg,sizeof(movq_reg),0,scratch,sizeof(scratch),&block)==PW_ERR_UNSUPPORTED);
    assert(pw_x86_translate(movss_reg,sizeof(movss_reg),0,scratch,sizeof(scratch),&block)==PW_ERR_UNSUPPORTED);
    /*
     * "bt [ebx], eax": the bit index and the guard's address register are the
     * same guest register, so the block has to keep the guest's index and the
     * validated address in different host registers. The index lives in EAX
     * here, which is also where memory_address_width() computes the address,
     * so this encoding is the one that would break if the two were confused.
     */
    {
        const uint8_t bt_mem[]={0x0f,0xa3,0x03};      /* bt dword [ebx], eax */
        const uint32_t window=0x03000800u;
        const PwX86Memory saved_memory=state.memory[0];
        const uint32_t saved_count=state.memory_count;

        state.memory_count=1;
        state.memory[0]=(PwX86Memory){state.stack_low,state.stack_high,
                                      PW_X86_READ|PW_X86_WRITE};
        *(uint32_t *)(uintptr_t)window=0x00000004u;      /* bit 2 of unit 0 */
        *(uint32_t *)(uintptr_t)(window+4u)=0u;          /* bit 0 of unit 1 */
        for(unsigned mode=0;mode<4;mode++) {
            /* The matrix's own initial state: every GPR carries a distinct
             * pattern, including a guest ESP the block must not need. */
            for(unsigned reg=0;reg<8;reg++)
                state.gpr[reg]=reg==3u?window:0x11111111u*(reg+1u);
            state.gpr[0]=2u;                 /* eax: bit 2 is set */
            state.eflags=0x202u;
            assert(run_mode(bt_mem,sizeof(bt_mem),0x9600,mode&1u,mode>>1)==0);
            assert((state.eflags&0x1u)!=0u);
            state.gpr[0]=3u;                 /* eax: bit 3 is clear */
            state.eflags=0x202u;
            assert(run_mode(bt_mem,sizeof(bt_mem),0x9600,mode&1u,mode>>1)==0);
            assert((state.eflags&0x1u)==0u);
        }
        /*
         * A bit offset of 32 on a dword operand addresses the *next* dword,
         * which is what the ISA means by a bit string: the guard has to cover
         * the unit the CPU really reads, not the base the ModRM names.
         */
        *(uint32_t *)(uintptr_t)window=0u;
        *(uint32_t *)(uintptr_t)(window+4u)=0x00000001u;
        for(unsigned mode=0;mode<4;mode++) {
            for(unsigned reg=0;reg<8;reg++)
                state.gpr[reg]=reg==3u?window:0x11111111u*(reg+1u);
            state.gpr[0]=32u;                /* eax: bit 0 of the next dword */
            state.eflags=0x202u;
            assert(run_mode(bt_mem,sizeof(bt_mem),0x9620,mode&1u,mode>>1)==0);
            assert((state.eflags&0x1u)!=0u);
        }
        /*
         * And an offset far outside the declared region is a *refused* call,
         * not a host fault: this is the bypass the SSE form matrix found, so
         * it is the case that must stay pinned. 0x11111111 selects the unit
         * at window + 0x02222220.
         */
        for(unsigned mode=0;mode<4;mode++) {
            for(unsigned reg=0;reg<8;reg++)
                state.gpr[reg]=reg==3u?window:0x11111111u*(reg+1u);
            state.gpr[0]=0x11111111u;
            state.eflags=0x202u;
            assert(run_mode(bt_mem,sizeof(bt_mem),0x9640,mode&1u,mode>>1)==-1);
            assert(state.gpr[3]==window);
        }
        /* The immediate form moves the unit too: "bt dword [ebx], 40" is bit
         * 8 of the next dword, and the immediate is masked to the bit
         * position inside that unit. */
        {
            const uint8_t bt_imm[]={0x0f,0xba,0x23,40};  /* bt [ebx], 40 */

            *(uint32_t *)(uintptr_t)window=0u;
            *(uint32_t *)(uintptr_t)(window+4u)=0x00000100u;
            state.gpr[3]=window;
            state.eflags=0x202u;
            assert(run(bt_imm,sizeof(bt_imm),0x9660)==0);
            assert((state.eflags&0x1u)!=0u);
        }
        state.memory[0]=saved_memory;
        state.memory_count=saved_count;
    }
    /*
     * "66 0f d6" with a register operand moves the low 64 bits into the r/m
     * register and zeroes its upper half; it is not a store. Emitting it as a
     * store wrote the window's address as data - the form matrix caught it as a
     * translated window holding another register's value.
     */
    {
        const uint8_t movq_reg_store[]={0x66,0x0f,0xd6,0xd3}; /* movq xmm3,xmm2 */
        const uint32_t window=0x03000800u;
        const PwX86Memory saved_memory=state.memory[0];
        const uint32_t saved_count=state.memory_count;
        uint32_t window_before[4];

        memcpy(window_before,(void *)(uintptr_t)window,sizeof(window_before));
        state.memory_count=1;
        state.memory[0]=(PwX86Memory){state.stack_low,state.stack_high,
                                      PW_X86_READ|PW_X86_WRITE};
        memset(state.fp.xmm,0,sizeof(state.fp.xmm));
        for(unsigned unit=0;unit<16;unit++) {
            state.fp.xmm[2][unit]=(uint8_t)(0x20u+unit);
            state.fp.xmm[3][unit]=(uint8_t)(0x30u+unit);
        }
        state.gpr[3]=window;
        assert(run(movq_reg_store,sizeof(movq_reg_store),0x9800)==0);
        for(unsigned unit=0;unit<8;unit++)
            assert(state.fp.xmm[3][unit]==(uint8_t)(0x20u+unit));
        for(unsigned unit=8;unit<16;unit++)
            assert(state.fp.xmm[3][unit]==0u);          /* upper half zeroed */
        for(unsigned unit=0;unit<16;unit++)
            assert(state.fp.xmm[2][unit]==(uint8_t)(0x20u+unit));
        assert(memcmp(window_before,(void *)(uintptr_t)window,
                      sizeof(window_before))==0);       /* no store happened */
        state.memory[0]=saved_memory;
        state.memory_count=saved_count;
    }
    /*
     * PMOVMSKB's r/m operand is always an XMM register - the ISA has no memory
     * form - so a ModRM that names memory has to be refused rather than
     * re-emitted. The SSE form matrix (tests/test_pw_sse_matrix.py) found the
     * accepted version as a sigill inside the translated block, with no
     * guest-visible fault; the register form keeps working and keeps its
     * lane-to-bit order.
     */
    {
        const uint8_t pmovmskb_reg[]={0x66,0x0f,0xd7,0xd3};
        const uint8_t pmovmskb_mem[]={0x66,0x0f,0xd7,0x03};
        static const uint8_t lanes[16]={0x80,0x01,0xff,0x00,0x00,0x80,0x7f,0x80,
                                        0x00,0x00,0x00,0x00,0x81,0x02,0x03,0x80};

        memcpy(state.fp.xmm[3],lanes,sizeof(lanes));
        state.gpr[2]=0;
        assert(run(pmovmskb_reg,sizeof(pmovmskb_reg),0x8400)==0);
        assert(state.gpr[2]==0x90a5u);
        assert(pw_x86_translate(pmovmskb_mem,sizeof(pmovmskb_mem),0,scratch,
                                sizeof(scratch),&block)==PW_ERR_UNSUPPORTED);
        assert(block.code_bytes==0);
    }
    state.memory_count=0;
    state.gpr[0]=0x50000000;set_xmm(0,1,2,3,4);
    assert(run(movups_oob,sizeof(movups_oob),0x8300)==-1);
    assert(state.fault_address==0x50000000 && state.fault_width==16);
}

static void bit_and_cmov_tests(void)
{
    const uint32_t low=state.stack_low,high=state.stack_high;
    const uint8_t bt_reg[]={0x0f,0xa3,0xc8};         /* bt eax, ecx */
    const uint8_t bts_reg[]={0x0f,0xab,0xc8};        /* bts eax, ecx */
    const uint8_t btr_reg[]={0x0f,0xb3,0xc8};        /* btr eax, ecx */
    const uint8_t btc_reg[]={0x0f,0xbb,0xc8};        /* btc eax, ecx */
    const uint8_t bt_memory[]={0x0f,0xa3,0x08};      /* bt [eax], ecx */
    const uint8_t bts_memory[]={0x0f,0xab,0x08};     /* bts [eax], ecx */

    state.memory_count=0;
    /* BT defines CF only: the flags the ISA leaves undefined stay put. */
    state.gpr[0]=0x00000008;state.gpr[1]=3;state.eflags=0x202;
    assert(run(bt_reg,sizeof(bt_reg),0x7100)==0);
    assert((state.eflags&1)==1 && state.gpr[0]==0x00000008);
    assert((state.eflags&0x8d4)==(0x202u&0x8d4u));
    state.gpr[1]=2;state.eflags=0x202;
    assert(run(bt_reg,sizeof(bt_reg),0x7101)==0);
    assert((state.eflags&1)==0);
    /* A register destination masks the index to five bits. */
    state.gpr[1]=35;state.eflags=0x202;
    assert(run(bt_reg,sizeof(bt_reg),0x7102)==0);
    assert((state.eflags&1)==1);
    /* BTS/BTR/BTC modify the destination and report the previous bit. */
    state.gpr[0]=0x100;state.gpr[1]=4;state.eflags=0x202;
    assert(run(bts_reg,sizeof(bts_reg),0x7110)==0);
    assert(state.gpr[0]==0x110 && (state.eflags&1)==0);
    state.gpr[0]=0x110;state.gpr[1]=4;state.eflags=0x202;
    assert(run(bts_reg,sizeof(bts_reg),0x7111)==0);
    assert(state.gpr[0]==0x110 && (state.eflags&1)==1);
    state.gpr[0]=0x110;state.gpr[1]=4;state.eflags=0x202;
    assert(run(btr_reg,sizeof(btr_reg),0x7120)==0);
    assert(state.gpr[0]==0x100 && (state.eflags&1)==1);
    state.gpr[0]=0x100;state.gpr[1]=4;state.eflags=0x202;
    assert(run(btc_reg,sizeof(btc_reg),0x7130)==0);
    assert(state.gpr[0]==0x110 && (state.eflags&1)==0);
    /* Immediate-index forms. */
    const uint8_t bt_imm[]={0x0f,0xba,0xe0,0x07};    /* bt eax, 7 */
    const uint8_t bts_imm[]={0x0f,0xba,0xe8,0x07};   /* bts eax, 7 */
    state.gpr[0]=0x80;state.eflags=0x202;
    assert(run(bt_imm,sizeof(bt_imm),0x7140)==0);
    assert((state.eflags&1)==1);
    state.gpr[0]=0;assert(run(bts_imm,sizeof(bts_imm),0x7141)==0);
    assert(state.gpr[0]==0x80);
    /* A memory destination uses bit-string addressing: bit 40 of a dword
     * operand is bit 8 of the next dword, not a mask of the first one. */
    memset((void *)(uintptr_t)low,0,8);
    state.gpr[0]=low;state.gpr[1]=40;state.eflags=0x202;
    assert(run(bts_memory,sizeof(bts_memory),0x7150)==0);
    uint32_t first=0,second=0;
    memcpy(&first,(void *)(uintptr_t)low,4);
    memcpy(&second,(void *)(uintptr_t)(low+4),4);
    assert(first==0 && second==0x100 && (state.eflags&1)==0);
    /* The 16-bit register form masks the index to four bits. */
    const uint8_t bt16[]={0x66,0x0f,0xa3,0xc8};      /* bt ax, cx */
    state.gpr[0]=0x8000;state.gpr[1]=15;state.eflags=0x202;
    assert(run(bt16,sizeof(bt16),0x7160)==0);
    assert((state.eflags&1)==1);
    state.gpr[1]=16;state.eflags=0x202;              /* masked to bit 0 */
    assert(run(bt16,sizeof(bt16),0x7161)==0);
    assert((state.eflags&1)==0);
    /* CMOVcc writes the destination only when the condition holds, and it
     * never writes flags. */
    const uint8_t cmovb[]={0x0f,0x42,0xc8};          /* cmovb ecx, eax */
    state.gpr[0]=0x1111;state.gpr[1]=0x2222;state.eflags=0x202|1;
    assert(run(cmovb,sizeof(cmovb),0x7170)==0);
    assert(state.gpr[1]==0x1111 && state.eflags==(0x202|1));
    state.gpr[0]=0x3333;state.gpr[1]=0x2222;state.eflags=0x202;
    assert(run(cmovb,sizeof(cmovb),0x7171)==0);
    assert(state.gpr[1]==0x2222 && state.eflags==0x202);
    uint32_t memory_value=0xabcdef01;
    memcpy((void *)(uintptr_t)low,&memory_value,4);
    const uint8_t cmovb_mem[]={0x0f,0x42,0x08};      /* cmovb ecx, [eax] */
    state.gpr[0]=low;state.gpr[1]=0x55;state.eflags=0x202|1;
    assert(run(cmovb_mem,sizeof(cmovb_mem),0x7180)==0);
    assert(state.gpr[1]==0xabcdef01);
    /* Refused forms: the 16-bit conditional move and the undefined 0f ba
     * sub-opcodes, plus a truncated immediate. */
    uint8_t scratch[4096];PwX86Block block;
    const uint8_t cmov16[]={0x66,0x0f,0x42,0xc1};
    const uint8_t ba_bad[]={0x0f,0xba,0xc0,0x01};
    const uint8_t ba_truncated[]={0x0f,0xba,0xe0};
    assert(pw_x86_translate(cmov16,sizeof(cmov16),0,scratch,sizeof(scratch),&block)==PW_ERR_UNSUPPORTED);
    assert(pw_x86_translate(ba_bad,sizeof(ba_bad),0,scratch,sizeof(scratch),&block)==PW_ERR_UNSUPPORTED);
    assert(pw_x86_translate(ba_truncated,sizeof(ba_truncated),0,scratch,sizeof(scratch),&block)==PW_ERR_TRUNCATED);
    /* The same two families through every engine mode: chaining is a
     * separate switch exercised by the engine tests, while residency and
     * lazy flags change the emitted code for these instructions. */
    for(unsigned residency=0;residency<2;residency++)
        for(unsigned lazy=0;lazy<2;lazy++) {
            state.gpr[0]=0x1111;state.gpr[1]=0x2222;state.eflags=0x202|1;
            assert(run_mode(cmovb,sizeof(cmovb),0x71a0,residency,lazy)==0);
            assert(state.gpr[1]==0x1111 && state.eflags==(0x202|1));
            state.gpr[0]=0x3333;state.gpr[1]=0x2222;state.eflags=0x202;
            assert(run_mode(cmovb,sizeof(cmovb),0x71b0,residency,lazy)==0);
            assert(state.gpr[1]==0x2222 && state.eflags==0x202);
            state.gpr[0]=0x100;state.gpr[1]=4;state.eflags=0x202;
            assert(run_mode(bts_reg,sizeof(bts_reg),0x71c0,residency,lazy)==0);
            assert(state.gpr[0]==0x110 && (state.eflags&1)==0);
            state.gpr[0]=0x80;state.eflags=0x202;
            assert(run_mode(bt_imm,sizeof(bt_imm),0x71d0,residency,lazy)==0);
            assert((state.eflags&1)==1);
            memset((void *)(uintptr_t)low,0,8);
            state.gpr[0]=low;state.gpr[1]=40;state.eflags=0x202;
            assert(run_mode(bts_memory,sizeof(bts_memory),0x71e0,residency,lazy)==0);
            uint32_t tail_low=0,tail_high=0;
            memcpy(&tail_low,(void *)(uintptr_t)low,4);
            memcpy(&tail_high,(void *)(uintptr_t)(low+4),4);
            assert(tail_low==0 && tail_high==0x100);
        }
    /* A memory read-modify-write needs write permission; a memory BT does
     * not, so it still works in the same state. */
    state.stack_low=state.stack_high=0;
    state.memory_count=1;
    state.memory[0]=(PwX86Memory){low,high,PW_X86_READ};
    state.gpr[0]=low;state.gpr[1]=1;
    assert(run(bt_memory,sizeof(bt_memory),0x7190)==0);
    assert(run(bts_memory,sizeof(bts_memory),0x7191)==-1);
    state.memory_count=0;
    state.stack_low=low;state.stack_high=high;
}

static void comparison_tests(void)
{
    state.gpr[0]=0x12348000;state.gpr[1]=0xffff8000;state.eflags=0xad7;
    const uint8_t test_ax_cx[]={0x66,0x85,0xc8};
    assert(run(test_ax_cx,sizeof(test_ax_cx),0x7ff)==PW_OK);
    assert(state.gpr[0]==0x12348000 && state.gpr[1]==0xffff8000 && state.eflags==0x296);
    for(unsigned flags=0;flags<32;flags++)for(unsigned condition=0;condition<16;condition++) {
        unsigned cf=flags&1,pf=(flags>>1)&1,zf=(flags>>2)&1,sf=(flags>>3)&1,of=(flags>>4)&1;
        unsigned expected[]={of,!of,cf,!cf,zf,!zf,cf||zf,!(cf||zf),sf,!sf,pf,!pf,sf!=of,sf==of,zf||(sf!=of),!zf&&(sf==of)};
        state.eflags=0x202|cf|(pf<<2)|(zf<<6)|(sf<<7)|(of<<11);
        uint32_t saved_flags=state.eflags;
        const uint8_t branch[]={(uint8_t)(0x70+condition),0xfe};
        assert(run(branch,2,0x1000)==0 && state.eip==(expected[condition]?0x1000:0x1002));
        const uint8_t near_branch[]={0x0f,(uint8_t)(0x80+condition),0xfa,0xff,0xff,0xff};
        assert(run(near_branch,6,0x2000)==0 && state.eip==(expected[condition]?0x2000:0x2006));
        for(unsigned reg=0;reg<8;reg++) {
            state.gpr[reg&3]=0xaabbccdd;
            const uint8_t set[]={0x0f,(uint8_t)(0x90+condition),(uint8_t)(0xc0|reg)};
            assert(run(set,3,0x3000)==0);
            unsigned shift=reg>=4?8:0;
            assert(state.gpr[reg&3]==((0xaabbccddu&~(255u<<shift))|(expected[condition]<<shift)));
        }
        assert(state.eflags==saved_flags);
    }
    state.gpr[0]=state.stack_high-2;
    uint16_t word=0xbeef;memcpy((void *)(uintptr_t)state.gpr[0],&word,2);
    uint32_t saved_eax=state.gpr[0];state.eflags=0x202;
    const uint8_t cmp16[]={0x66,0x81,0x38,0xef,0xbe};
    assert(run(cmp16,sizeof(cmp16),0x4000)==0 && state.eflags==0x246 && state.gpr[0]==saved_eax);
    const uint8_t cmp32[]={0x81,0x38,0xef,0xbe,0,0};
    assert(run(cmp32,sizeof(cmp32),0x4010)==-1 && state.eflags==0x246);
    const uint8_t extend[]={0x0f,0xb7,0x10};
    assert(run(extend,3,0x4020)==0 && state.gpr[2]==0xbeef && state.eflags==0x246);
    state.gpr[0]=0xffffffff;
    const uint8_t sign32[]={0x83,0xf8,0xff},sign16[]={0x66,0x83,0xf8,0xff};
    assert(run(sign32,3,0x4030)==0 && state.eflags==0x246);
    assert(run(sign16,4,0x4040)==0 && state.eflags==0x246);
    state.gpr[0]=3;state.gpr[1]=state.stack_low;
    uint32_t five=5;memcpy(stack.write_base,&five,4);
    const uint8_t cmp_reg_mem[]={0x3b,0x01},cmp_mem_reg[]={0x39,0x01};
    assert(run(cmp_reg_mem,2,0x4050)==0 && (state.eflags&1));
    assert(run(cmp_mem_reg,2,0x4060)==0 && !(state.eflags&1));
    for(unsigned direction=0;direction<2;direction++) {
        state.gpr[0]=0xffffffff;state.gpr[1]=1;
        const uint8_t add[]={direction?0x03:0x01,direction?0xc1:0xc8};
        assert(run(add,2,0x4070)==0 && state.gpr[0]==0 && (state.eflags&0x8d5)==0x55);
    }
}

/*
 * Every 32-bit ModRM/SIB address form, through LEA edx: mod 0-2, each rm,
 * each SIB byte, disp8 and disp32 with both signs, register values that
 * wrap past 4 GiB, in all four engine modes. The expected address is the
 * architectural sum modulo 2^32, computed here in C.
 */
static void address_form_tests(void)
{
    static const uint32_t displacements[] = { 0x7f, 0xffffff80u, 0x12345678u, 0xfffffff0u };
    static const uint32_t register_sets[][8] = {
        { 0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u, 0x55555555u, 0x66666666u,
          0x77777777u, 0x88888888u },
        { 0xfffffff0u, 0x10u, 0x80000000u, 0x7fffffffu, 0xffffffffu, 1u, 0xc0000000u,
          0x3fffffffu },
    };
    const PwX86State saved = state;
    unsigned checked = 0;

    for (unsigned mode = 0; mode < 4; mode++)
    for (unsigned set = 0; set < 2; set++)
    for (unsigned mod = 0; mod < 3; mod++)
    for (unsigned rm = 0; rm < 8; rm++)
    for (unsigned sib = 0; sib < (rm == 4 ? 256u : 1u); sib++)
    for (unsigned d = 0; d < 4; d++) {
        const unsigned base = rm == 4 ? (sib & 7) : rm, index = (sib >> 3) & 7, scale = sib >> 6;
        const int no_base = mod == 0 && base == 5;
        const unsigned disp_bytes = mod == 1 ? 1 : (mod == 2 || no_base) ? 4 : 0;
        uint8_t op[8];
        size_t n = 0;
        uint32_t displacement = 0, expected = 0;

        if (mod == 1 && d >= 2) continue;          /* disp8 takes the first two */
        if (!disp_bytes && d) continue;
        op[n++] = 0x8d;
        op[n++] = (uint8_t)((mod << 6) | (2u << 3) | rm);  /* lea edx, ... */
        if (rm == 4) op[n++] = (uint8_t)sib;
        if (disp_bytes == 1) {
            displacement = displacements[d];
            op[n++] = (uint8_t)displacement;
        } else if (disp_bytes == 4) {
            displacement = displacements[d];
            memcpy(op + n, &displacement, 4);
            n += 4;
        }
        memcpy(state.gpr, register_sets[set], sizeof(state.gpr));
        expected = displacement;
        if (!no_base) expected += register_sets[set][base];
        if (rm == 4 && index != 4) expected += register_sets[set][index] << scale;
        state.eflags = 0x2;
        assert(run_mode(op, n, 0xd400, mode & 1, mode >> 1) == 0);
        if (state.gpr[2] != expected) {
            fprintf(stderr, "lea form mod %u rm %u sib %02x disp %08x mode %u: %08x, want %08x\n",
                    mod, rm, sib, displacement, mode, state.gpr[2], expected);
            assert(0);
        }
        checked++;
    }
    assert(checked > 4000);
    state = saved;
}

static void prefixed_padding_tests(void)
{
    static const uint8_t forms[][15] = {
        {0x90}, {0x66,0x66,0x90}, {0x0f,0x1f,0xc0},
        {0x66,0x0f,0x1f,0x44,0,0},
        {0x66,0x66,0x66,0x66,0x66,0x66,0x2e,0x0f,0x1f,0x84,0,0,0,0,0},
        {0x67,0x0f,0x1f,0x06,0,0},
        {0x67,0x0f,0x1f,0x80,0,0},
        {0x64,0x65,0x2e,0x66,0x0f,0x1f,0x00}
    };
    static const unsigned lengths[] = {1,3,3,6,15,6,6,7};
    PwX86Block block;
    uint8_t output[4096], overlong[16];
    uintptr_t before, after;
    __asm__ volatile("pushfq; pop %0; .byte 0x66,0x66,0x66,0x66,0x66,0x66,0x2e,0x0f,0x1f,0x84,0,0,0,0,0; pushfq; pop %1"
                     : "=r"(before), "=r"(after) : : "memory");
    assert(before == after);
    for (unsigned f = 0; f < sizeof(lengths)/sizeof(lengths[0]); f++) {
        assert(pw_x86_padding_length(forms[f], lengths[f]) == (int)lengths[f]);
        for (unsigned n = 1; n < lengths[f]; n++)
            assert(pw_x86_translate(forms[f],n,0,output,sizeof(output),&block)==PW_ERR_TRUNCATED);
        for (unsigned mode = 0; mode < 4; mode++) {
            PwX86State saved;
            state.gpr[0] = 0; /* a NOP's nominal memory operand is unmapped */
            state.memory_count = 0; state.eflags = 0xad7;
            saved = state;
            assert(run_mode(forms[f],lengths[f],0x9000,mode&1,mode>>1)==0);
            assert(!memcmp(state.gpr,saved.gpr,sizeof(state.gpr)));
            assert(state.eflags==saved.eflags && state.eip==0x9000+lengths[f]);
        }
    }
    memset(overlong,0x66,8); memcpy(overlong+8,forms[4]+7,8);
    assert(pw_x86_translate(overlong,sizeof(overlong),0,output,sizeof(output),&block)==PW_ERR_UNSUPPORTED);
    {
        const uint8_t bad[]={0x66,0x0f,0x1f,0xc8};
        const uint8_t lock[]={0xf0,0x0f,0x1f,0x00};
        const uint8_t other[]={0x66,0x89,0xc1}, pause[]={0xf3,0x90};
        assert(pw_x86_translate(bad,sizeof(bad),0,output,sizeof(output),&block)==PW_ERR_UNSUPPORTED);
        assert(pw_x86_translate(lock,sizeof(lock),0,output,sizeof(output),&block)==PW_ERR_UNSUPPORTED);
        assert(!pw_x86_padding_length(other,sizeof(other)));
        assert(!pw_x86_padding_length(pause,sizeof(pause)));
    }
}

int main(int argc, char **argv)
{
    assert(pw_vm_posix_backend(&backend)==PW_OK);
    assert(backend.reserve(NULL,8192,4096,&code)==PW_OK);
    assert(backend.reserve_at(NULL,0x03000000,4096,4096,&stack)==PW_OK);
    assert(backend.commit(NULL,&stack,0,stack.bytes,PW_PROT_READ|PW_PROT_WRITE)==PW_OK);
    state.stack_low=0x03000000; state.stack_high=0x03001000;
    state.gpr[4]=state.stack_high;
    /* The form matrix is a mode of its own: it prints the initial state and
     * the state after every accepted form for the native oracle in
     * tests/test_pw_sse_matrix.py to compare against, then returns. */
    if (argc==2 && strcmp(argv[1],"--sse-matrix")==0)
        return sse_matrix();
    string_tests();
    prefixed_padding_tests();
    muldiv_tests();
    address_form_tests();
    /* Independent reference in test_pw_x86_reference.S executes these
     * operations as 32-bit instructions on the host CPU. */
    const uint8_t input[]={0x6a,0xff,0x68,0x44,0x33,0x22,0x11,0xe8,0,0,0,0};
    assert(run(input,sizeof(input),0x01000000)==0);
    assert(state.eip==0x0100000c && state.gpr[4]==state.stack_high-12);
    uint32_t *top=(uint32_t *)(uintptr_t)state.gpr[4];
    assert(top[0]==0x0100000c && top[1]==0x11223344 && top[2]==0xffffffffu);
    if (argc==2 && strcmp(argv[1],"--emit")==0)
        assert(fwrite(top,4,3,stdout)==3);
    const uint8_t address_reference[]={0xb8,0xf0,0xff,0xff,0xff,
        0xb9,3,0,0,0,0x8d,0x54,0xc8,0x20};
    assert(run(address_reference,sizeof(address_reference),0x800)==0);
    assert(state.gpr[2]==0x28);
    if (argc==2 && strcmp(argv[1],"--emit")==0)
        assert(fwrite(&state.gpr[2],4,1,stdout)==1);
    /* FS-prefixed absolute operands, the same shape the native reference in
     * tests/test_pw_x86_reference.S executes with a real 32-bit FS base. */
    {
        const uint8_t fs_load_edi[]={0x64,0x8b,0x3d,0x18,0,0,0};
        const uint8_t fs_store_edi[]={0x64,0x89,0x3d,0x1c,0,0,0};
        const uint8_t fs_load_esi[]={0x64,0x8b,0x35,0x1c,0,0,0};
        PwVmRegion fs_block;
        uint32_t written=0x11223344u;

        assert(backend.reserve_at(NULL,0x03004000,4096,4096,&fs_block)==PW_OK);
        assert(backend.commit(NULL,&fs_block,0,fs_block.bytes,
                              PW_PROT_READ|PW_PROT_WRITE)==PW_OK);
        state.fs_base=0x03004000;state.fs_bytes=4096;
        memcpy((uint8_t *)(uintptr_t)state.fs_base+0x18,&written,4);
        assert(run(fs_load_edi,sizeof(fs_load_edi),0x900)==0);
        assert(run(fs_store_edi,sizeof(fs_store_edi),0x910)==0);
        assert(run(fs_load_esi,sizeof(fs_load_esi),0x920)==0);
        assert(state.gpr[7]==written && state.gpr[6]==written);
        /*
         * "call dword ptr fs:[disp32]": Wine's other syscall stub shape. The
         * target comes from the guest's own FS block (TEB.WOW32Reserved,
         * where the unix side installs the dispatcher) and the guest return
         * address is pushed exactly as an indirect call pushes it, so the
         * engine ends the block at the target.
         */
        {
            const uint8_t fs_call[]={0x64,0xff,0x15,0xc0,0,0,0};
            uint32_t target=0x00123456u,pushed=0u;
            const uint32_t saved_esp=state.gpr[4];
            const uint32_t scratch_address=state.stack_high-0x104u;
            uint32_t saved=0u;

            memcpy(&saved,(void *)(uintptr_t)scratch_address,4);
            memcpy((uint8_t *)(uintptr_t)state.fs_base+0xc0,&target,4);
            state.gpr[4]=state.stack_high-0x100u;
            assert(run(fs_call,sizeof(fs_call),0x930)==0);
            assert(state.eip==target);
            assert(state.gpr[4]==scratch_address);
            memcpy(&pushed,(void *)(uintptr_t)scratch_address,4);
            assert(pushed==0x00000937u);
            /* Leave the stack exactly as the later tests expect it. */
            memcpy((void *)(uintptr_t)scratch_address,&saved,4);
            state.gpr[4]=saved_esp;
        }
        if (argc==2 && strcmp(argv[1],"--emit")==0) {
            assert(fwrite(&state.gpr[7],4,1,stdout)==1);
            assert(fwrite(&state.gpr[6],4,1,stdout)==1);
        }
        assert(backend.release(NULL,&fs_block)==PW_OK);
    }
    /* LOCK-prefixed read-modify-write, the same shape the native reference
     * executes with a real "lock addl $1, mem". */
    {
        const uint8_t lock_add[]={0xf0,0x83,0x05,0,0,0,0,0x01};
        uint8_t instruction[sizeof(lock_add)];
        uint32_t five=5,result=0,address=state.stack_low;

        memcpy(instruction,lock_add,sizeof(lock_add));
        memcpy(instruction+3,&address,4);
        memcpy((void *)(uintptr_t)address,&five,4);
        assert(run(instruction,sizeof(instruction),0xa00)==0);
        memcpy(&result,(void *)(uintptr_t)address,4);
        assert(result==5+1);
        if (argc==2 && strcmp(argv[1],"--emit")==0)
            assert(fwrite(&result,4,1,stdout)==1);
    }
    /*
     * LOCK CMPXCHG to memory, the same two shapes the native reference in
     * tests/test_pw_x86_reference.S executes: one where the word equals the
     * accumulator (the word takes the source, EAX does not move, ZF is set)
     * and one where it does not (the word keeps its value and EAX takes what
     * was there). Each prints the word, the accumulator and the arithmetic
     * flags, in the reference's order.
     */
    {
        PwVmRegion slot;
        const uint8_t equal_case[]={
            0xb9,0x00,0x00,0x10,0x03,           /* mov ecx, 0x03100000 */
            0xc7,0x01,0x44,0x33,0x22,0x11,      /* mov [ecx], 0x11223344 */
            0xb8,0x44,0x33,0x22,0x11,           /* mov eax, 0x11223344 */
            0xba,0xdd,0xcc,0xbb,0xaa,           /* mov edx, 0xaabbccdd */
            0xf0,0x0f,0xb1,0x11,                /* lock cmpxchg [ecx], edx */
        };
        const uint8_t unequal_case[]={
            0xc7,0x01,0x44,0x33,0x22,0x11,      /* mov [ecx], 0x11223344 */
            0xb8,0x88,0x77,0x66,0x55,           /* mov eax, 0x55667788 */
            0xba,0x11,0x11,0x11,0x11,           /* mov edx, 0x11111111 */
            0xf0,0x0f,0xb1,0x11,                /* lock cmpxchg [ecx], edx */
        };

        assert(backend.reserve_at(NULL,0x03100000,4096,4096,&slot)==PW_OK);
        assert(backend.commit(NULL,&slot,0,slot.bytes,
                              PW_PROT_READ|PW_PROT_WRITE)==PW_OK);
        state.memory_count=1;
        state.memory[0]=(PwX86Memory){0x03100000,0x03101000,
                                      PW_X86_READ|PW_X86_WRITE};
        assert(run(equal_case,sizeof(equal_case),0x02100000)==0);
        if (argc==2 && strcmp(argv[1],"--emit")==0) {
            uint32_t word=0,accumulator=state.gpr[0];
            uint32_t flags=state.eflags&0x8d5u;

            memcpy(&word,(const void *)(uintptr_t)0x03100000,4);
            assert(fwrite(&word,4,1,stdout)==1);
            assert(fwrite(&accumulator,4,1,stdout)==1);
            assert(fwrite(&flags,4,1,stdout)==1);
        }
        assert(run(unequal_case,sizeof(unequal_case),0x02200000)==0);
        if (argc==2 && strcmp(argv[1],"--emit")==0) {
            uint32_t word=0,accumulator=state.gpr[0];
            uint32_t flags=state.eflags&0x8d5u;

            memcpy(&word,(const void *)(uintptr_t)0x03100000,4);
            assert(fwrite(&word,4,1,stdout)==1);
            assert(fwrite(&accumulator,4,1,stdout)==1);
            assert(fwrite(&flags,4,1,stdout)==1);
        }
        state.memory_count=0;
        assert(backend.release(NULL,&slot)==PW_OK);
    }
    /*
     * The accumulator's byte forms, the same shape the native reference
     * executes: "mov [disp32], al" stores only the low byte and
     * "mov al, [disp32]" replaces only it, so the upper 24 bits of the guest
     * accumulator survive both.
     */
    {
        PwVmRegion slot;
        const uint8_t byte_forms[]={
            0xb8,0xaa,0x33,0x22,0x11,      /* mov eax, 0x112233aa */
            0xa2,0x00,0x00,0x20,0x03,      /* mov [0x03200000], al */
            0xb8,0x00,0x77,0x66,0x55,      /* mov eax, 0x55667700 */
            0xa0,0x00,0x00,0x20,0x03,      /* mov al, [0x03200000] */
        };

        assert(backend.reserve_at(NULL,0x03200000,4096,4096,&slot)==PW_OK);
        assert(backend.commit(NULL,&slot,0,slot.bytes,
                              PW_PROT_READ|PW_PROT_WRITE)==PW_OK);
        state.memory_count=1;
        state.memory[0]=(PwX86Memory){0x03200000,0x03201000,
                                      PW_X86_READ|PW_X86_WRITE};
        assert(run(byte_forms,sizeof(byte_forms),0x02300000)==0);
        assert(state.gpr[0]==0x556677aau);
        if (argc==2 && strcmp(argv[1],"--emit")==0)
            assert(fwrite(&state.gpr[0],4,1,stdout)==1);
        state.memory_count=0;
        assert(backend.release(NULL,&slot)==PW_OK);
    }
    /* Bit test and conditional move, the same shape the native reference
     * executes with "btsl %ecx, %eax" and "cmovel %esi, %edx". */
    {
        const uint8_t bts[]={0x0f,0xab,0xc8};         /* bts eax, ecx */
        const uint8_t cmove[]={0x0f,0x44,0xd6};       /* cmove edx, esi */
        state.gpr[0]=0x100;state.gpr[1]=4;state.eflags=0x202;
        assert(run(bts,sizeof(bts),0xb00)==0);
        assert(state.gpr[0]==0x110);
        state.gpr[2]=0x55;state.gpr[6]=0xaa;state.eflags=0x202|0x40;
        assert(run(cmove,sizeof(cmove),0xb10)==0);
        assert(state.gpr[2]==0xaa);
        if (argc==2 && strcmp(argv[1],"--emit")==0) {
            assert(fwrite(&state.gpr[0],4,1,stdout)==1);
            assert(fwrite(&state.gpr[2],4,1,stdout)==1);
        }
    }
    /*
     * CMOVcc with a memory source: the condition has to survive the memory
     * guard, which computes the address in EAX and uses EDX as its scratch
     * register. When the condition was kept in EDX the guard overwrote it and
     * every memory form moved unconditionally - the form matrix reported
     * exactly the conditions that are false with EFLAGS zero. Both outcomes
     * are pinned here, in all four engine modes.
     */
    {
        const uint8_t cmove_mem[]={0x0f,0x44,0x03};   /* cmove eax, [ebx] */
        const uint32_t window=0x03000800u;
        const PwX86Memory saved_memory=state.memory[0];
        const uint32_t saved_count=state.memory_count;

        state.memory_count=1;
        state.memory[0]=(PwX86Memory){state.stack_low,state.stack_high,
                                      PW_X86_READ|PW_X86_WRITE};
        *(uint32_t *)(uintptr_t)window=0xaabbccddu;
        for(unsigned mode=0;mode<4;mode++) {
            /* ZF set: the condition is true, so the source is selected. */
            state.gpr[0]=0x11111111u;state.gpr[3]=window;
            state.eflags=0x202u|0x40u;
            assert(run_mode(cmove_mem,sizeof(cmove_mem),0x9700,mode&1u,mode>>1)==0);
            assert(state.gpr[0]==0xaabbccddu);
            /* ZF clear: the condition is false, so the destination stays. */
            state.gpr[0]=0x11111111u;state.gpr[3]=window;
            state.eflags=0x202u;
            assert(run_mode(cmove_mem,sizeof(cmove_mem),0x9710,mode&1u,mode>>1)==0);
            assert(state.gpr[0]==0x11111111u);
        }
        state.memory[0]=saved_memory;
        state.memory_count=saved_count;
    }
    /*
     * XCHG to memory, the same shape the native reference in
     * tests/test_pw_x86_reference.S executes: the operand and the register
     * exchange values, and the instruction writes no flags at all - so the
     * flags "xor eax, eax" left behind (ZF and PF, which the mask below keeps)
     * must still be there afterwards. The three words printed here are the
     * flags after the exchange, the register and the word, in the native
     * program's order: a translation that only stored, that left the register
     * alone, or that announced flags of its own differs on one of them.
     */
    {
        PwVmRegion slot;
        const uint8_t exchange[]={
            0xc7,0x01,0x44,0x33,0x22,0x11,      /* mov [ecx], 0x11223344 */
            0xba,0xdd,0xcc,0xbb,0xaa,           /* mov edx, 0xaabbccdd */
            0x31,0xc0,                          /* xor eax, eax: ZF and PF */
            0x87,0x11,                          /* xchg [ecx], edx */
        };

        assert(backend.reserve_at(NULL,0x03300000,4096,4096,&slot)==PW_OK);
        assert(backend.commit(NULL,&slot,0,slot.bytes,
                              PW_PROT_READ|PW_PROT_WRITE)==PW_OK);
        state.memory_count=1;
        state.memory[0]=(PwX86Memory){0x03300000,0x03301000,
                                      PW_X86_READ|PW_X86_WRITE};
        state.gpr[1]=0x03300000;
        assert(run(exchange,sizeof(exchange),0x02400000)==0);
        /* The word took the register and the register took the word. */
        assert(*(uint32_t *)(uintptr_t)(0x03300000+0x0)==0xaabbccddu);
        assert(state.gpr[2]==0x11223344u);
        /* And the flags the "xor eax, eax" left behind are untouched: 0x8d5
         * is the architectural flag mask, and ZF|PF is what the xor set. */
        assert((state.eflags&0x8d5u)==0x44u);
        if (argc==2 && strcmp(argv[1],"--emit")==0) {
            uint32_t flags=state.eflags&0x8d5u;

            assert(fwrite((void *)(uintptr_t)(0x03300000),4,1,stdout)==1);
            assert(fwrite(&state.gpr[2],4,1,stdout)==1);
            assert(fwrite(&flags,4,1,stdout)==1);
        }
        state.memory_count=0;
        assert(backend.release(NULL,&slot)==PW_OK);
    }
    /*
     * The SSE lane, executed here through the translator as one block and in
     * tests/test_pw_x86_reference.S as native i386. Every register the
     * sequence reads is written first, and the bytes emitted below are the
     * stored lanes, the lane mask and the extracted word - the same values the
     * native program prints, from a different implementation.
     */
    {
        static const uint8_t sse_in[16] = {
            0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
            0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,
        };
        const uint32_t in_address=state.stack_low+0x200u;
        const uint32_t out_address=state.stack_low+0x300u;
        const uint8_t sse_sequence[]={
            0xf3,0x0f,0x6f,0x06,             /* movdqu xmm0,[esi] */
            0xb8,0x02,0x00,0x01,0x00,         /* mov eax,0x00010002 */
            0x66,0x0f,0x6e,0xc8,              /* movd xmm1,eax */
            0x66,0x0f,0xfd,0xc1,              /* paddw xmm0,xmm1 */
            0x66,0x0f,0x70,0xd0,0x00,         /* pshufd xmm2,xmm0,0 */
            0x66,0x0f,0x62,0xd2,              /* punpckldq xmm2,xmm2 */
            0x66,0x0f,0x6f,0xd8,              /* movdqa xmm3,xmm0 */
            0x66,0x0f,0xdb,0xda,              /* pand xmm3,xmm2 */
            0x0f,0x11,0x1f,                   /* movups [edi],xmm3 */
            0x66,0x0f,0xd7,0xd3,              /* pmovmskb edx,xmm3 */
            0x66,0x0f,0xc5,0xcb,0x03,         /* pextrw ecx,xmm3,3 */
        };

        memset((void *)(uintptr_t)in_address,0,32u);
        memcpy((void *)(uintptr_t)in_address,sse_in,sizeof(sse_in));
        memset((void *)(uintptr_t)out_address,0,32u);
        memset(state.fp.xmm,0,sizeof(state.fp.xmm));
        state.gpr[6]=in_address;                  /* esi */
        state.gpr[7]=out_address;                 /* edi */
        assert(run(sse_sequence,sizeof(sse_sequence),0xc00)==0);
        if (argc==2 && strcmp(argv[1],"--emit")==0) {
            assert(fwrite((void *)(uintptr_t)out_address,16,1,stdout)==1);
            assert(fwrite(&state.gpr[2],4,1,stdout)==1);
            assert(fwrite(&state.gpr[1],4,1,stdout)==1);
        }
    }
    const uint8_t ret[]={0xc3};
    assert(run(ret,1,0x02000000)==0);
    assert(state.eip==0x0100000c && state.gpr[4]==state.stack_high-8);
    state.gpr[4]=state.stack_low;
    memset(stack.write_base,0x5a,stack.bytes);
    assert(run(input,sizeof(input),0x01000000)==-1);
    assert(state.gpr[4]==state.stack_low && state.eip==0x01000000);
    for(size_t i=0;i<stack.bytes;i++)assert(((uint8_t *)stack.write_base)[i]==0x5a);
    state.gpr[4]=state.stack_high;
    assert(run(ret,1,0x02000000)==-1);
    assert(state.gpr[4]==state.stack_high);
    /* Register encodings, preserving all other registers, including ESP. */
    for (unsigned reg=0;reg<8;reg++) {
        state.gpr[4]=state.stack_high;
        for (unsigned i=0;i<8;i++) if(i!=4)state.gpr[i]=0xaabb0000+i;
        uint32_t before[8]; memcpy(before,state.gpr,sizeof(before));
        const uint8_t push[]={(uint8_t)(0x50+reg)};
        assert(run(push,1,0x100)==0);
        assert(*(uint32_t *)(uintptr_t)state.gpr[4]==before[reg]);
        const uint8_t pop[]={(uint8_t)(0x58+reg)};
        assert(run(pop,1,0x101)==0);
        assert(memcmp(before,state.gpr,sizeof(before))==0);
        assert(state.eip==0x102);
    }
    /* POP ESP uses the loaded value, not the old ESP plus four. */
    state.gpr[4]=state.stack_high-4;
    *(uint32_t *)(uintptr_t)state.gpr[4]=state.stack_low+128;
    const uint8_t popesp[]={0x5c};
    assert(run(popesp,1,0x200)==0);
    assert(state.gpr[4]==state.stack_low+128);
    /* Failure after one successful instruction commits exactly that prefix. */
    state.gpr[4]=state.stack_low+4;
    const uint8_t twice[]={0x50,0x51};
    assert(run(twice,2,0x300)==-1);
    assert(state.gpr[4]==state.stack_low && state.eip==0x301);
    assert(*(uint32_t *)stack.write_base==state.gpr[0]);
    /* All register/register MOV combinations, including ESP. */
    for (unsigned direction=0;direction<2;direction++)
        for(unsigned src=0;src<8;src++)for(unsigned dst=0;dst<8;dst++) {
            for(unsigned i=0;i<8;i++)state.gpr[i]=0x12340000+i;
            uint32_t expected[8];memcpy(expected,state.gpr,sizeof(expected));
            expected[dst]=expected[src];
            uint8_t mov[]={direction?0x8b:0x89,
                (uint8_t)(0xc0 | ((direction?dst:src)<<3) | (direction?src:dst))};
            assert(run(mov,2,0x400)==0);
            assert(memcmp(expected,state.gpr,sizeof(expected))==0);
        }
    PwVmRegion thread;
    assert(backend.reserve_at(NULL,0x03002000,4096,4096,&thread)==PW_OK);
    assert(backend.commit(NULL,&thread,0,thread.bytes,PW_PROT_READ|PW_PROT_WRITE)==PW_OK);
    state.fs_base=0x03002000;state.fs_bytes=4096;
    const uint8_t fsread[]={0x64,0xa1,0,0,0,0};
    const uint8_t fswrite[]={0x64,0xa3,0,0,0,0};
    *(uint32_t *)thread.write_base=0xffffffffu;
    assert(run(fsread,sizeof(fsread),0x500)==0);
    assert(state.gpr[0]==0xffffffffu && state.eip==0x506);
    state.gpr[0]=0x03000100;
    assert(run(fswrite,sizeof(fswrite),0x510)==0);
    assert(*(uint32_t *)thread.write_base==0x03000100);
    /* FS-prefixed absolute dword operands. Wine's ntdll begins with
     * "mov edi, fs:[0x18]" (the TEB self pointer), so the general form has
     * to work in both directions, through the guest's own FS base. */
    const uint8_t fsload_edi[]={0x64,0x8b,0x3d,0x18,0,0,0};
    const uint8_t fsstore_ecx[]={0x64,0x89,0x0d,0x1c,0,0,0};
    *(uint32_t *)((uint8_t *)thread.write_base+0x18)=0x11223344u;
    state.gpr[7]=0;state.eflags=0xad7;
    assert(run(fsload_edi,sizeof(fsload_edi),0x570)==0);
    assert(state.gpr[7]==0x11223344u && state.eip==0x577);
    assert(state.eflags==0xad7);        /* mov defines no flags */
    state.gpr[1]=0x55667788u;
    assert(run(fsstore_ecx,sizeof(fsstore_ecx),0x580)==0);
    assert(*(uint32_t *)((uint8_t *)thread.write_base+0x1c)==0x55667788u);
    /* The last dword inside the block is still addressable. */
    const uint8_t fsload_last[]={0x64,0x8b,0x15,0xfc,0x0f,0,0};
    memcpy((uint8_t *)thread.write_base+4092,&state.gpr[0],4);
    state.gpr[2]=0;
    assert(run(fsload_last,sizeof(fsload_last),0x585)==0);
    assert(state.gpr[2]==state.gpr[0]);
    /* The FS block's own size is the bound: an offset outside it is refused
     * before any dereference, and the block is left untouched. */
    const uint32_t before_write=*(uint32_t *)((uint8_t *)thread.write_base+0x1c);
    state.fs_bytes=0x1cu;
    assert(run(fsstore_ecx,sizeof(fsstore_ecx),0x590)==-1);
    assert(*(uint32_t *)((uint8_t *)thread.write_base+0x1c)==before_write);
    /* The last dword at 0x18 is exactly the end of that shorter block, so
     * the load still succeeds while the store above did not. */
    assert(run(fsload_edi,sizeof(fsload_edi),0x591)==0);
    assert(state.gpr[7]==0x11223344u);
    state.fs_bytes=4096;
    assert(run(fsstore_ecx,sizeof(fsstore_ecx),0x592)==0);
    /* Past the end of the FS block: rejected before any dereference. */
    const uint8_t fsload_oob[]={0x64,0x8b,0x3d,0x00,0x10,0,0};
    state.gpr[7]=0x11111111u;
    assert(run(fsload_oob,sizeof(fsload_oob),0x5a0)==-1);
    assert(state.gpr[7]==0x11111111u && state.eip==0x5a0);
    uint8_t last[]={0x64,0xa3,0xfc,0x0f,0,0};
    assert(run(last,sizeof(last),0x520)==0);
    assert(*(uint32_t *)((uint8_t *)thread.write_base+4092)==state.gpr[0]);
    last[2]=0xfd;
    assert(run(last,sizeof(last),0x530)==-1 && state.eip==0x530);
    assert(state.gpr[0]==0x03000100);
    state.fs_bytes=3;
    assert(run(fsread,sizeof(fsread),0x540)==-1);
    assert(state.gpr[0]==0x03000100);
    state.fs_base=0xfffffffdu;state.fs_bytes=4096;
    assert(run(fsread,sizeof(fsread),0x550)==-1);
    /* Base+offset overflow fails before dereferencing address zero. */
    const uint8_t overflow[]={0x64,0xa1,4,0,0,0};
    assert(run(overflow,sizeof(overflow),0x560)==-1);
    /* General MOV memory uses explicit read/write permissions, not merely
     * presence of a live mapping. */
    state.memory_count=1;
    state.memory[0]=(PwX86Memory){0x03002000,0x03003000,PW_X86_READ};
    const uint8_t read_region[]={0x8b,0x05,0,0x20,0,3};
    const uint8_t write_region[]={0x89,0x05,0,0x20,0,3};
    assert(run(read_region,sizeof(read_region),0xb00)==0);
    uint32_t saved=*(uint32_t *)thread.write_base;
    state.gpr[0]=0x12345678;
    assert(run(write_region,sizeof(write_region),0xb10)==-1);
    assert(*(uint32_t *)thread.write_base==saved);
    state.memory[0].permissions=PW_X86_WRITE;
    assert(run(write_region,sizeof(write_region),0xb20)==0);
    assert(*(uint32_t *)thread.write_base==0x12345678);
    assert(run(read_region,sizeof(read_region),0xb30)==-1);
    state.memory[0].permissions=PW_X86_READ;
    state.memory[0].high=0x03002003;
    assert(run(read_region,sizeof(read_region),0xb40)==-1);
    state.memory_count=PW_X86_MEMORY_REGIONS+1;
    assert(run(read_region,sizeof(read_region),0xb50)==-1);
    state.memory_count=0;
    assert(run(read_region,sizeof(read_region),0xb60)==-1);
    assert(backend.release(NULL,&thread)==PW_OK);
    x87_transfer_tests();
    addressing_tests();
    arithmetic_tests();
    comparison_tests();
    immediate_tests();
    absolute_tests();
    lock_prefix_tests();
    bit_and_cmov_tests();
    sse_and_scan_tests();
    optimization_safety_tests();
    push_operand_tests();
    logical_test_tests();
    arithmetic_memory_tests();
    unary_leave_tests();
    byte_tests();
    ret_cleanup_tests();
    extension_tests();
    shift_tests();
    rotate_tests();
    double_shift_tests();
    /* Enter an actual translated guest callback, then restore its caller. */
    state.gpr[4]=state.stack_high-16;state.eip=0xf0000010;
    PwX86State caller=state;PwGuestCallback callback={0};
    assert(pw_guest_callback_enter(&callback,&state,0x1000,0xf1000010,NULL,0,PW_GUEST_CDECL)==PW_OK);
    const uint8_t callback_code[]={0xb8,42,0,0,0,0xc3};
    assert(run(callback_code,sizeof(callback_code),0x1000)==0);
    uint64_t callback_result=0;
    assert(pw_guest_callback_leave(&callback,32,&callback_result)==PW_OK);
    assert(callback_result==42 && memcmp(&caller,&state,sizeof(state))==0);
    state.gpr[4]=state.stack_high;state.gpr[7]=0xe0000120;
    const uint8_t indirect_call[]={0xff,0xd7};
    assert(run(indirect_call,2,0x2000)==0);
    assert(state.eip==0xe0000120 && state.gpr[4]==state.stack_high-4);
    assert(*(uint32_t *)(uintptr_t)state.gpr[4]==0x2002);
    state.gpr[0]=state.gpr[4];
    const uint8_t indirect_jump[]={0xff,0x20};
    assert(run(indirect_jump,2,0x2100)==0 && state.eip==0x2002);
    assert(state.gpr[4]==state.stack_high-4);
    state.gpr[4]=state.stack_low;
    assert(run(indirect_call,2,0x2200)==-1 && state.eip==0x2200);
    uint8_t scratch[4096]; PwX86Block block;
    const uint8_t fs[]={0x64,0x90};
    /* A segment override on NOP is ignored; it does not read FS memory. */
    assert(pw_x86_translate(fs,sizeof(fs),0,scratch,sizeof(scratch),&block)==PW_OK);
    assert(block.source_bytes==sizeof(fs));
    /* Only FS absolute dword operands are supported: GS, an FS memory
     * operand with a base or index, and byte-width FS forms stay refused
     * rather than being translated as something else. */
    const uint8_t gs_load[]={0x65,0x8b,0x3d,0x18,0,0,0};
    const uint8_t fs_indexed[]={0x64,0x8b,0x3c,0x0d,0x18,0,0,0};
    const uint8_t fs_base_only[]={0x64,0x8b,0x38};
    const uint8_t fs_byte[]={0x64,0x8a,0x3d,0x18,0,0,0};
    const uint8_t fs_unterminated[]={0x64,0x8b,0x3d,0x18,0,0};
    assert(pw_x86_translate(gs_load,sizeof(gs_load),0,scratch,sizeof(scratch),&block)==PW_ERR_UNSUPPORTED);
    assert(pw_x86_translate(fs_indexed,sizeof(fs_indexed),0,scratch,sizeof(scratch),&block)==PW_ERR_UNSUPPORTED);
    assert(pw_x86_translate(fs_base_only,sizeof(fs_base_only),0,scratch,sizeof(scratch),&block)==PW_ERR_UNSUPPORTED);
    assert(pw_x86_translate(fs_byte,sizeof(fs_byte),0,scratch,sizeof(scratch),&block)==PW_ERR_UNSUPPORTED);
    assert(pw_x86_translate(fs_unterminated,sizeof(fs_unterminated),0,scratch,sizeof(scratch),&block)==PW_ERR_TRUNCATED);
    assert(pw_x86_translate(fsread,5,0,scratch,sizeof(scratch),&block)==PW_ERR_TRUNCATED);
    assert(pw_x86_translate(input,1,0,scratch,sizeof(scratch),&block)==PW_ERR_TRUNCATED);
    assert(pw_x86_translate(input,sizeof(input),0,scratch,1,&block)==PW_ERR_LIMIT);
    assert(backend.release(NULL,&code)==PW_OK);
    assert(backend.release(NULL,&stack)==PW_OK);
    return 0;
}
