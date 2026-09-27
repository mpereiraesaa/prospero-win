/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include "../src/pw_x86_hostexec.h"
#include "../src/pw_vm_posix.h"
#include "../include/prospero_win.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <xmmintrin.h>

static uint8_t *guest;           /* identity-mapped below 4 GiB */
static PwVmBackend vm;
static PwX86HostExec hx;

static void reset_state(PwX86State *s)
{
    memset(s, 0, sizeof(*s));
    pw_guest_fp_init(&s->fp);
    s->eip = 0x1000;
    s->eflags = 0x2;
    s->gpr[4] = (uint32_t)(uintptr_t)(guest + 0x8000);
    s->fs_base = (uint32_t)(uintptr_t)(guest + 0x4000);
}

static int run(PwX86State *s, const uint8_t *code, size_t n)
{
    return pw_x86_hostexec_step(&hx, s, code, n);
}

static uint32_t addr(size_t offset) { return (uint32_t)(uintptr_t)(guest + offset); }

static void test_integer_forms(void)
{
    PwX86State s;
    uint8_t code[16];
    uint32_t target = addr(0x100);

    /* 66 A3 moffs32: mov [moffs], ax — the first Wine frontier. */
    reset_state(&s);
    s.gpr[0] = 0xdeadbeef;
    memset(guest + 0x100, 0x11, 4);
    code[0] = 0x66; code[1] = 0xa3; memcpy(code + 2, &target, 4);
    assert(run(&s, code, 6) == PW_OK);
    assert(guest[0x100] == 0xef && guest[0x101] == 0xbe && guest[0x102] == 0x11);
    assert(s.eip == 0x1006);

    /* 66 C1 C0 08: rol ax, 8 swaps the low word's bytes, keeps the high word. */
    reset_state(&s);
    s.gpr[0] = 0x12345678;
    assert(run(&s, (const uint8_t[]){0x66, 0xc1, 0xc0, 0x08}, 4) == PW_OK);
    assert(s.gpr[0] == 0x12347856 && s.eip == 0x1004);

    /* F6 10: not byte [eax]. */
    reset_state(&s);
    s.gpr[0] = addr(0x200);
    guest[0x200] = 0x0f;
    assert(run(&s, (const uint8_t[]){0xf6, 0x10}, 2) == PW_OK);
    assert(guest[0x200] == 0xf0);

    /* 66 A9 imm16: test ax, 0xfffd sets ZF only on a zero result. */
    reset_state(&s);
    s.gpr[0] = 0x00020002;
    assert(run(&s, (const uint8_t[]){0x66, 0xa9, 0xfd, 0xff}, 4) == PW_OK);
    assert((s.eflags & 0x40) == 0x40 && s.eip == 0x1004);

    /* 8B 44 24 04: mov eax, [esp+4] computes the address from guest ESP. */
    reset_state(&s);
    memcpy(guest + 0x8004, &(uint32_t){0xcafef00d}, 4);
    assert(run(&s, (const uint8_t[]){0x8b, 0x44, 0x24, 0x04}, 4) == PW_OK);
    assert(s.gpr[0] == 0xcafef00d);

    /* 64 A1 18000000: mov eax, fs:[0x18] reads from the guest FS base. */
    reset_state(&s);
    memcpy(guest + 0x4018, &(uint32_t){0x7ffd0000}, 4);
    assert(run(&s, (const uint8_t[]){0x64, 0xa1, 0x18, 0, 0, 0}, 6) == PW_OK);
    assert(s.gpr[0] == 0x7ffd0000 && s.eip == 0x1006);

    /* 64 8B 35 30000000: mov esi, fs:[0x30] — ESI is the destination, so
     * the effective address travels in EDI and ESI receives the value. */
    reset_state(&s);
    s.gpr[7] = 0x77777777;
    memcpy(guest + 0x4030, &(uint32_t){0x00251000}, 4);
    assert(run(&s, (const uint8_t[]){0x64, 0x8b, 0x35, 0x30, 0, 0, 0}, 7) == PW_OK);
    assert(s.gpr[6] == 0x00251000 && s.gpr[7] == 0x77777777);

    /* 64 FF 35 00000000 / 64 8F 05 00000000: the SEH push/pop fs:[0] pair. */
    reset_state(&s);
    memcpy(guest + 0x4000, &(uint32_t){0x0022f000}, 4);
    assert(run(&s, (const uint8_t[]){0x64, 0xff, 0x35, 0, 0, 0, 0}, 7) == PW_OK);
    assert(s.gpr[4] == addr(0x7ffc) && s.eip == 0x1007);
    assert(!memcmp(guest + 0x7ffc, &(uint32_t){0x0022f000}, 4));
    memcpy(guest + 0x7ffc, &(uint32_t){0x0022e000}, 4);
    assert(run(&s, (const uint8_t[]){0x64, 0x8f, 0x05, 0, 0, 0, 0}, 7) == PW_OK);
    assert(s.gpr[4] == addr(0x8000));
    assert(!memcmp(guest + 0x4000, &(uint32_t){0x0022e000}, 4));
    /* pop [esp] addresses with the incremented ESP. */
    memcpy(guest + 0x7ffc, &(uint32_t){0x13572468}, 4);
    s.gpr[4] = addr(0x7ffc);
    assert(run(&s, (const uint8_t[]){0x8f, 0x04, 0x24}, 3) == PW_OK);
    assert(!memcmp(guest + 0x8000, &(uint32_t){0x13572468}, 4));

    /* 9C/9D: pushfd stores the guest flags with bit 1 and IF; the CPUID
     * probe (pushfd; pushfd; btc [esp], 21; popfd; pushfd; pop eax) sees ID
     * toggle, and popfd restores the arithmetic and DF bits it pushed. */
    reset_state(&s);
    s.eflags = 0x2 | 0x400 | 0x41;                          /* DF, ZF, CF */
    assert(run(&s, (const uint8_t[]){0x9c}, 1) == PW_OK);
    assert(s.gpr[4] == addr(0x7ffc) && s.eip == 0x1001);
    assert(!memcmp(guest + 0x7ffc, &(uint32_t){0x643}, 4));
    memcpy(guest + 0x7ffc, &(uint32_t){0x00200000u | 0x100 | 0x3000 | 0x880 | 0x2}, 4);
    assert(run(&s, (const uint8_t[]){0x9d}, 1) == PW_OK);
    assert(s.gpr[4] == addr(0x8000) && s.eip == 0x1002);
    assert(s.eflags == (0x00200000u | 0x880 | 0x2));        /* ID, OF, SF; no TF or IOPL */
    assert(run(&s, (const uint8_t[]){0x9c}, 1) == PW_OK);
    assert(!memcmp(guest + 0x7ffc, &(uint32_t){0x00200a82u}, 4));
    /* 66 9C (pushf, 16-bit) stays refused. */
    assert(run(&s, (const uint8_t[]){0x66, 0x9c}, 2) == PW_ERR_UNSUPPORTED);

    /* 98: cwde, and CF from a memory ADD, merged into the guest flags. */
    reset_state(&s);
    s.gpr[0] = 0x0000ff80;
    assert(run(&s, (const uint8_t[]){0x98}, 1) == PW_OK && s.gpr[0] == 0xffffff80);
    reset_state(&s);
    s.gpr[3] = addr(0x300);
    memcpy(guest + 0x300, &(uint32_t){0xffffffff}, 4);
    s.gpr[0] = 1;
    assert(run(&s, (const uint8_t[]){0x01, 0x03}, 2) == PW_OK);  /* add [ebx], eax */
    assert((s.eflags & 1) == 1 && (s.eflags & 0x40) == 0x40);
}

static void test_fp_forms(void)
{
    PwX86State s;
    static const uint8_t one[10] = {0, 0, 0, 0, 0, 0, 0, 0x80, 0xff, 0x3f};
    static const uint8_t two[10] = {0, 0, 0, 0, 0, 0, 0, 0x80, 0x00, 0x40};
    uint8_t got[10];
    double value = 7.9;
    uint16_t cw = 0x0c7f;

    /* D9 C9: fxch st(1). */
    reset_state(&s);
    assert(pw_guest_x87_push(&s.fp, one) == PW_OK);
    assert(pw_guest_x87_push(&s.fp, two) == PW_OK);
    assert(run(&s, (const uint8_t[]){0xd9, 0xc9}, 2) == PW_OK);
    assert(pw_guest_x87_peek(&s.fp, 0, got) == PW_OK && !memcmp(got, one, 10));
    assert(pw_guest_x87_peek(&s.fp, 1, got) == PW_OK && !memcmp(got, two, 10));

    /* DF F1: fcomip st(1) with st0=1 < st1=2 sets CF and pops once. */
    assert(run(&s, (const uint8_t[]){0xdf, 0xf1}, 2) == PW_OK);
    assert((s.eflags & 1) == 1 && (s.eflags & 0x40) == 0);
    assert(pw_guest_x87_peek(&s.fp, 0, got) == PW_OK && !memcmp(got, two, 10));

    /* D9 28: fldcw [eax] updates only the guest control word. */
    reset_state(&s);
    s.gpr[0] = addr(0x400);
    memcpy(guest + 0x400, &cw, 2);
    assert(run(&s, (const uint8_t[]){0xd9, 0x28}, 2) == PW_OK);
    assert(s.fp.x87_control == 0x0c7f);

    /* F2 0F 2C 00: cvttsd2si eax, [eax] truncates toward zero. */
    reset_state(&s);
    s.gpr[0] = addr(0x500);
    memcpy(guest + 0x500, &value, 8);
    assert(run(&s, (const uint8_t[]){0xf2, 0x0f, 0x2c, 0x00}, 4) == PW_OK);
    assert(s.gpr[0] == 7);

    /* 66 0F 72 F1 03: pslld xmm1, 3 in the guest register file. */
    reset_state(&s);
    for (unsigned i = 0; i < 4; i++) memcpy(s.fp.xmm[1] + i * 4, &(uint32_t){i + 1}, 4);
    assert(run(&s, (const uint8_t[]){0x66, 0x0f, 0x72, 0xf1, 0x03}, 5) == PW_OK);
    for (unsigned i = 0; i < 4; i++) {
        uint32_t lane;
        memcpy(&lane, s.fp.xmm[1] + i * 4, 4);
        assert(lane == (i + 1) << 3);
    }
}

static void test_refusals_and_cache(void)
{
    PwX86State s;
    PwX86HostExecPlan plan;
    static const uint8_t refused[][4] = {
        {0xe8, 0, 0, 0}, {0x50}, {0x89, 0xe0}, {0x65, 0xa1, 0, 0}, {0x67, 0x8b, 0x00},
        {0x8d, 0x00}, {0xff, 0x10}, {0xcd, 0x2e}, {0x40}, {0x61}, {0x94}, {0x0f, 0x0b},
    };
    uint16_t host_cw_before, host_cw_after;
    unsigned mxcsr_before, mxcsr_after;
    uint64_t compiled;

    reset_state(&s);
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++)
        assert(pw_x86_hostexec_plan(&s, refused[i], 4, &plan) == PW_ERR_UNSUPPORTED);
    assert(pw_x86_hostexec_plan(&s, (const uint8_t[]){0x66, 0xa3, 0}, 3, &plan) == PW_ERR_TRUNCATED);

    /* The guest's FPU/MXCSR state never leaks into the host. */
    __asm__ volatile("fnstcw %0" : "=m"(host_cw_before));
    mxcsr_before = _mm_getcsr();
    s.fp.x87_control = 0x0c7f;
    s.fp.mxcsr = 0x7f80;
    assert(run(&s, (const uint8_t[]){0xd9, 0xc9}, 2) == PW_OK);
    __asm__ volatile("fnstcw %0" : "=m"(host_cw_after));
    mxcsr_after = _mm_getcsr();
    assert(host_cw_before == host_cw_after && mxcsr_before == mxcsr_after);
    assert(s.fp.x87_control == 0x0c7f && s.fp.mxcsr == 0x7f80);

    /* A repeated PC reuses its stub; different bytes at that PC recompile. */
    reset_state(&s);
    compiled = hx.compiled;
    s.eip = 0x2000;
    assert(run(&s, (const uint8_t[]){0x98}, 1) == PW_OK);
    s.eip = 0x2000;
    assert(run(&s, (const uint8_t[]){0x98}, 1) == PW_OK);
    assert(hx.compiled == compiled + 1);
    s.eip = 0x2000;
    assert(run(&s, (const uint8_t[]){0x99}, 1) == PW_OK);
    assert(hx.compiled == compiled + 2);
    assert(pw_x86_hostexec_reset(&hx) == PW_OK && hx.cursor == 16);
}

int main(void)
{
    guest = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    assert(guest != MAP_FAILED && (uintptr_t)guest < 0x100000000ull);
    assert(pw_vm_posix_backend(&vm) == PW_OK);
    assert(pw_x86_hostexec_init(&hx, &vm, 1u << 20) == PW_OK);
    test_integer_forms();
    test_fp_forms();
    test_refusals_and_cache();
    assert(pw_x86_hostexec_destroy(&hx) == PW_OK);
    printf("host-exec fallback passed: integer, x87, SSE, FS and SIB forms; %llu stubs\n",
           (unsigned long long)hx.compiled);
    return 0;
}
