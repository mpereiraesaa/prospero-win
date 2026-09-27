/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * The software x87 against the host's own x87 unit, for the guest control
 * words a Windows program uses: every rounding mode, the three precision
 * settings (Windows starts threads at 53 bits, 0x027f), and operands that are
 * denormal, round to a denormal, sit exactly halfway, overflow or are
 * inexact. Each case runs the same operation natively under the same control
 * word and compares the 80-bit result and the status word bit for bit.
 * Exceptions stay masked, as they are by default on Windows.
 */
#include "../src/pw_x87.h"
#include "../include/prospero_win.h"
#include <stdio.h>
#include <string.h>

#if defined(__x86_64__) || defined(__i386__)
typedef struct Native { uint8_t st0[10]; uint16_t status; } Native;

/* The chained forms divide first, so the second operation sees a full
 * 64-bit significand at 64-bit precision. */
enum { OP_ADD, OP_SUB, OP_SUBR, OP_MUL, OP_DIV, OP_DIVR, OP_SQRT, OP_ST32, OP_ST64, OP_LD32,
       OP_DIV_MUL, OP_DIV_ADD, OP_DIV_SQRT, OP_COUNT };
static const char *const names[OP_COUNT] = {
    "fadd", "fsub", "fsubr", "fmul", "fdiv", "fdivr", "fsqrt", "fst m32", "fst m64", "fld m32",
    "fdiv; fmul", "fdiv; fadd", "fdiv; fsqrt",
};
/* Status bits compared: the six exception flags, stack fault, C1 (the
 * rounded-up bit an inexact result reports) and TOP. */
enum { STATUS_MASK = 0x387f };

static void native(uint16_t control, unsigned op, uint64_t a, uint64_t b, Native *out, uint64_t *stored)
{
    uint8_t st0[10] = {0};
    uint16_t status = 0;
    uint64_t result = 0;

    __asm__ volatile("fninit\n\tfldcw %0" : : "m"(control));
    switch (op) {
    case OP_LD32: {
        uint32_t value = (uint32_t)a;
        __asm__ volatile("flds %0" : : "m"(value));
        break;
    }
    default:
        __asm__ volatile("fldl %0" : : "m"(a));
        break;
    }
    switch (op) {
    case OP_ADD: __asm__ volatile("faddl %0" : : "m"(b)); break;
    case OP_SUB: __asm__ volatile("fsubl %0" : : "m"(b)); break;
    case OP_SUBR: __asm__ volatile("fsubrl %0" : : "m"(b)); break;
    case OP_MUL: __asm__ volatile("fmull %0" : : "m"(b)); break;
    case OP_DIV: __asm__ volatile("fdivl %0" : : "m"(b)); break;
    case OP_DIVR: __asm__ volatile("fdivrl %0" : : "m"(b)); break;
    case OP_SQRT: __asm__ volatile("fsqrt"); break;
    case OP_DIV_MUL: __asm__ volatile("fdivl %0\n\tfmull %1" : : "m"(b), "m"(a)); break;
    case OP_DIV_ADD: __asm__ volatile("fdivl %0\n\tfaddl %1" : : "m"(b), "m"(a)); break;
    case OP_DIV_SQRT: __asm__ volatile("fdivl %0\n\tfsqrt" : : "m"(b)); break;
    case OP_ST32: {
        uint32_t narrow;
        __asm__ volatile("fsts %0" : "=m"(narrow));
        result = narrow;
        break;
    }
    case OP_ST64: __asm__ volatile("fstl %0" : "=m"(result)); break;
    default: break;
    }
    __asm__ volatile("fnstsw %0" : "=m"(status));
    __asm__ volatile("fstpt %0\n\tfninit" : "=m"(st0));
    memcpy(out->st0, st0, 10);
    out->status = status;
    *stored = result;
}

static int emulated(uint16_t control, unsigned op, uint64_t a, uint64_t b, Native *out, uint64_t *stored)
{
    static const PwX87Action actions[OP_COUNT] = {
        PW_X87_FADD_F64, PW_X87_FSUB_F64, PW_X87_FSUBR_F64, PW_X87_FMUL_F64,
        PW_X87_FDIV_F64, PW_X87_FDIVR_F64, PW_X87_FSQRT, PW_X87_FST_F32, PW_X87_FST_F64, PW_X87_FLD_F32,
        PW_X87_FMUL_F64, PW_X87_FADD_F64, PW_X87_FSQRT,
    };
    PwGuestFp fp;
    uint32_t low = (uint32_t)a;
    uint64_t result = 0;
    uintptr_t operand = (uintptr_t)&b;
    int status;

    pw_guest_fp_init(&fp);
    fp.x87_control = control;
    status = op == OP_LD32 ? pw_x87_execute(&fp, PW_X87_FLD_F32, (uintptr_t)&low, NULL)
                           : pw_x87_execute(&fp, PW_X87_FLD_F64, (uintptr_t)&a, NULL);
    if (status != PW_OK) return status;
    if (op == OP_ST32 || op == OP_ST64) operand = (uintptr_t)&result;
    if (op >= OP_DIV_MUL) {
        if ((status = pw_x87_execute(&fp, PW_X87_FDIV_F64, (uintptr_t)&b, NULL)) != PW_OK) return status;
        operand = (uintptr_t)&a;
    }
    if (op != OP_LD32 && (status = pw_x87_execute(&fp, actions[op], operand, NULL)) != PW_OK)
        return status;
    if ((status = pw_guest_x87_peek(&fp, 0, out->st0)) != PW_OK) return status;
    out->status = fp.x87_status;
    *stored = result;
    return PW_OK;
}

static uint64_t rng = 0x243f6a8885a308d3ull;
static uint64_t next(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return rng;
}

int main(void)
{
    static const uint64_t fixed[] = {
        0x3ff0000000000000ull, /* 1 */
        0x3fd5555555555555ull, /* 1/3 */
        0x3ff0000000000001ull, /* 1 + ulp */
        0x4008000000000000ull, /* 3 */
        0x3fb999999999999aull, /* 0.1 */
        0x0010000000000000ull, /* smallest normal */
        0x000fffffffffffffull, /* largest denormal */
        0x0000000000000001ull, /* smallest denormal */
        0x8008000000000000ull, /* negative denormal */
        0x36a0000000000000ull, /* 2^-149, the smallest float denormal */
        0x3810000000000000ull, /* 2^-126, the smallest normal float */
        0x380fffffe0000000ull, /* rounds to a float denormal */
        0x47efffffe0000000ull, /* largest float */
        0x47efffffffffffffull, /* rounds past the largest float */
        0x7fefffffffffffffull, /* largest double */
        0x8000000000000000ull, /* -0 */
        0xc01c000000000000ull, /* -7 */
        0x3ff8000000000000ull, /* 1.5, halfway once narrowed */
    };
    enum { FIXED = sizeof(fixed) / sizeof(fixed[0]), RANDOM = 64 };
    uint64_t operands[FIXED + RANDOM];
    unsigned cases = 0, failures = 0;

    memcpy(operands, fixed, sizeof(fixed));
    for (unsigned i = 0; i < RANDOM; i++) {
        uint64_t bits = next();
        /* Half of them near the bottom of the range, where denormals are. */
        if (i & 1) bits &= 0x80ffffffffffffffull;
        operands[FIXED + i] = bits;
    }
    for (unsigned rc = 0; rc < 4; rc++)
        for (unsigned pc = 0; pc < 4; pc++) {
            if (pc == 1) continue; /* reserved */
            const uint16_t control = (uint16_t)(0x107fu & ~0x100u) | (uint16_t)(pc << 8) | (uint16_t)(rc << 10);
            for (unsigned op = 0; op < OP_COUNT; op++)
                for (unsigned i = 0; i < FIXED + RANDOM; i++)
                    for (unsigned j = 0; j < FIXED + RANDOM; j++) {
                        const int unary = op == OP_SQRT || op == OP_ST32 || op == OP_ST64 || op == OP_LD32;
                        uint64_t a = operands[i], b = operands[j], native_stored, emulated_stored = 0;
                        Native want, got;
                        int status;

                        if (unary && j) break;
                        /* NaN operands are outside this test (their payloads
                         * have their own). */
                        if (((a >> 52) & 0x7ff) == 0x7ff || ((b >> 52) & 0x7ff) == 0x7ff) continue;
                        if (op == OP_LD32) {
                            a = (uint32_t)(a >> 32);
                            if (((a >> 23) & 0xff) == 0xff) continue;
                        }
                        cases++;
                        native(control, op, a, b, &want, &native_stored);
                        status = emulated(control, op, a, b, &got, &emulated_stored);
                        if (status == PW_OK && !memcmp(want.st0, got.st0, 10) &&
                            !((want.status ^ got.status) & STATUS_MASK) &&
                            native_stored == emulated_stored)
                            continue;
                        if (failures++ < 40)
                            printf("MISMATCH %s cw=%04x a=%016llx b=%016llx: status %d native "
                                   "st0=%04x%016llx sw=%04x m=%016llx, emulated st0=%04x%016llx sw=%04x m=%016llx\n",
                                   names[op], control, (unsigned long long)a, (unsigned long long)b, status,
                                   want.st0[9] << 8 | want.st0[8], (unsigned long long)*(uint64_t *)want.st0,
                                   want.status, (unsigned long long)native_stored,
                                   got.st0[9] << 8 | got.st0[8], (unsigned long long)*(uint64_t *)got.st0,
                                   got.status, (unsigned long long)emulated_stored);
                    }
        }
    printf("x87 native comparison: %u cases, %u mismatches\n", cases, failures);
    return failures != 0;
}
#else
int main(void)
{
    puts("x87 native comparison: skipped, the host has no x87 unit");
    return 0;
}
#endif
