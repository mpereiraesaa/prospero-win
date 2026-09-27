/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Translator-versus-host differential over single instructions.
 *
 * Reads one hex instruction encoding per line (an optional second column is
 * ignored). For each encoding that both the translator and the host-executed
 * fallback accept, it runs a few randomized trials: the same initial guest
 * state and memory window are executed once as a one-instruction translated
 * block and once natively through pw_x86_hostexec, and the resulting GPRs,
 * EIP, arithmetic flags, memory window, x87 and SSE state are compared.
 *
 * Output: one line per mismatching encoding ("MISMATCH <hex> <what>") and a
 * final summary. Flags that the architecture leaves undefined for an
 * instruction class are masked (see undefined_flags).
 */
#define _GNU_SOURCE
#include "../src/pw_x86_engine.h"
#include "../src/pw_x86_hostexec.h"
#include "../src/pw_vm_posix.h"
#include "../include/prospero_win.h"
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

enum { WINDOW = 0x40000, CODE_AT = 0x3f000, STACK_AT = 0x30000, TRIALS = 4, ENTRIES = 256 };

static uint8_t *window, *snapshot, *after_translated;
/* Two extra pages mapped at an absolute effective address outside the window. */
static uint8_t *extra, extra_snapshot[0x2000], extra_translated[0x2000];
static const uint8_t *current_code;
static size_t current_len;
static sigjmp_buf fault_jump;
static volatile sig_atomic_t in_trial;

static void on_fault(int sig, siginfo_t *info, void *ctx)
{
    (void)sig; (void)info; (void)ctx;
    if (in_trial) siglongjmp(fault_jump, 1);
    _exit(3);
}

static int source_view(void *opaque, uint32_t pc, const uint8_t **source, size_t *bytes)
{
    (void)opaque;
    if (pc != (uint32_t)(uintptr_t)(window + CODE_AT)) return PW_ERR_NOT_FOUND;
    *source = current_code;
    *bytes = current_len;
    return PW_OK;
}

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t next_random(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)rng;
}

static int parse_hex(const char *text, uint8_t *out, size_t *len)
{
    size_t n = 0;
    while (text[0] && text[1] && text[0] != ' ' && text[0] != '\n') {
        unsigned v;
        if (n >= 15 || sscanf(text, "%2x", &v) != 1) return 0;
        out[n++] = (uint8_t)v;
        text += 2;
    }
    *len = n;
    return n > 0;
}

/* Flags the SDM leaves undefined, keyed by a coarse opcode class. */
static uint32_t undefined_flags(const uint8_t *code, size_t len)
{
    size_t i = 0;
    uint8_t op, sub = 0, reg;
    while (i < len && (code[i] == 0x66 || code[i] == 0xf2 || code[i] == 0xf3 ||
                       code[i] == 0xf0 || code[i] == 0x64 || code[i] == 0x2e ||
                       code[i] == 0x3e || code[i] == 0x26 || code[i] == 0x36)) i++;
    if (i >= len) return 0;
    op = code[i];
    reg = i + 1 < len ? (code[i + 1] >> 3) & 7 : 0;
    if (op == 0x0f && i + 1 < len) {
        sub = code[i + 1];
        reg = i + 2 < len ? (code[i + 2] >> 3) & 7 : 0;
        if (sub == 0xaf) return 0x0d4;                 /* IMUL: SF ZF AF PF */
        if (sub == 0xbc || sub == 0xbd) return 0x8d5;  /* BSF/BSR: all but ZF */
        if (sub == 0xa4 || sub == 0xa5 || sub == 0xac || sub == 0xad) return 0x810; /* SHLD/SHRD: OF AF */
        if (sub == 0xa3 || sub == 0xab || sub == 0xb3 || sub == 0xbb || sub == 0xba)
            return 0x8d4;                              /* BT*: OF SF ZF AF PF */
        return 0;
    }
    if ((op >= 0xd0 && op <= 0xd3) || op == 0xc0 || op == 0xc1)
        return 0x810;                                  /* shifts/rotates: OF (count>1), AF */
    if ((op == 0xf6 || op == 0xf7) && reg >= 4) return 0x8d5 & (reg >= 6 ? 0xfff : 0x0d4);
    if (op == 0x69 || op == 0x6b) return 0x0d4;
    if (op >= 0x20 && op <= 0x25) return 0x010;        /* AND: AF */
    if (op >= 0x08 && op <= 0x0d) return 0x010;        /* OR */
    if (op >= 0x30 && op <= 0x35) return 0x010;        /* XOR */
    if (op == 0x84 || op == 0x85 || op == 0xa8 || op == 0xa9) return 0x010;
    if ((op == 0xf6 || op == 0xf7) && reg <= 1) return 0x010;
    if ((op == 0x80 || op == 0x81 || op == 0x83) && (reg == 1 || reg == 4 || reg == 6)) return 0x010;
    return 0;
}

static void random_state(PwX86State *s, const uint8_t *code, size_t len)
{
    const uint32_t base = (uint32_t)(uintptr_t)window;
    memset(s, 0, sizeof(*s));
    for (unsigned r = 0; r < 8; r++)
        s->gpr[r] = base + 0x1000 + (next_random() % 0x20000);
    if (next_random() & 1) s->gpr[0] = next_random();
    s->gpr[1] = next_random() % 9;                     /* small counts for REP and CL */
    s->gpr[2] = next_random() & 0xffff;
    s->gpr[4] = base + STACK_AT;
    s->eip = base + CODE_AT;
    s->eflags = 0x2 | (next_random() & 0x8d5);
    s->fs_base = base + 0x2000;
    s->fs_bytes = 0x1000;
    s->stack_low = base + 0x1000;
    s->stack_high = base + WINDOW - 0x2000;
    s->memory_count = 1;
    s->memory[0].low = base + 0x1000;
    s->memory[0].high = (uint64_t)base + WINDOW - 0x2000;
    s->memory[0].permissions = PW_X86_READ | PW_X86_WRITE;
    pw_guest_fp_init(&s->fp);
    for (unsigned i = 0; i < 8; i++)
        for (unsigned b = 0; b < 16; b++) s->fp.xmm[i][b] = (uint8_t)next_random();
    /* Two finite x87 values so stack forms have operands. */
    {
        uint8_t v[10] = {0};
        for (unsigned k = 0; k < 2; k++) {
            uint64_t mant = 0x8000000000000000ull | ((uint64_t)next_random() << 20);
            uint16_t exp = (uint16_t)(0x3ff0 + (next_random() % 32));
            memcpy(v, &mant, 8); memcpy(v + 8, &exp, 2);
            pw_guest_x87_push(&s->fp, v);
        }
    }
    (void)code; (void)len;
}

static const char *compare(const PwX86State *a, const PwX86State *b, uint32_t mask)
{
    static char what[160];
    for (unsigned r = 0; r < 8; r++)
        if (a->gpr[r] != b->gpr[r]) {
            snprintf(what, sizeof(what), "gpr%u %08x!=%08x", r, a->gpr[r], b->gpr[r]);
            return what;
        }
    if (a->eip != b->eip) return "eip";
    if (((a->eflags ^ b->eflags) & 0x8d5 & ~mask) != 0) {
        snprintf(what, sizeof(what), "flags %03x!=%03x", a->eflags & 0x8d5, b->eflags & 0x8d5);
        return what;
    }
    if (memcmp(after_translated + 0x1000, window + 0x1000, WINDOW - 0x3000) != 0) return "memory";
    if (extra && memcmp(extra_translated, extra, 0x2000) != 0) return "memory-abs";
    if (memcmp(a->fp.xmm, b->fp.xmm, sizeof(a->fp.xmm)) != 0) return "xmm";
    if (a->fp.x87_control != b->fp.x87_control) return "fcw";
    if (((a->fp.x87_status ^ b->fp.x87_status) & 0x7d00) != 0) return "fsw-top/cc"; /* C1 is a rounding hint */
    for (unsigned i = 0; i < 8; i++) {
        unsigned sa = (a->fp.x87_status >> 11) & 7, sb = (b->fp.x87_status >> 11) & 7;
        unsigned ta = (a->fp.x87_tag >> (((sa + i) & 7) * 2)) & 3;
        unsigned tb = (b->fp.x87_tag >> (((sb + i) & 7) * 2)) & 3;
        if ((ta == 3) != (tb == 3)) return "x87-tag";
        if (ta != 3 && memcmp(a->fp.x87_st[(sa + i) & 7], b->fp.x87_st[(sb + i) & 7], 10) != 0) {
            const uint8_t *x = a->fp.x87_st[(sa + i) & 7], *y = b->fp.x87_st[(sb + i) & 7];
            snprintf(what, sizeof(what), "x87-value st%u %02x%02x:%02x%02x%02x%02x%02x%02x%02x%02x != %02x%02x:%02x%02x%02x%02x%02x%02x%02x%02x",
                     i, x[9], x[8], x[7], x[6], x[5], x[4], x[3], x[2], x[1], x[0],
                     y[9], y[8], y[7], y[6], y[5], y[4], y[3], y[2], y[1], y[0]);
            return what;
        }
    }
    return NULL;
}

/* Each protected run lives in its own frame so a fault's longjmp cannot
 * clobber the caller's locals. Returns 1 when the run faulted. */
static int run_translated(PwX86Engine *engine, PwX86State *t, int *status)
{
    PwX86StepReport report;

    in_trial = 1;
    if (sigsetjmp(fault_jump, 1)) { in_trial = 0; return 1; }
    *status = pw_x86_engine_step(engine, t, &report);
    in_trial = 0;
    return 0;
}

static int run_host(PwX86HostExec *hx, PwX86State *h, size_t len, int *status)
{
    in_trial = 1;
    if (sigsetjmp(fault_jump, 1)) { in_trial = 0; return 1; }
    *status = pw_x86_hostexec_step(hx, h, window + CODE_AT, len);
    in_trial = 0;
    return 0;
}

int main(int argc, char **argv)
{
    static PwX86CacheEntry entries[ENTRIES];
    static PwX86Engine engine;
    static PwX86HostExec hx;
    PwVmBackend vm;
    struct sigaction sa;
    char line[128];
    uint64_t tested = 0, skipped = 0, mismatched = 0, faults = 0;
    uint32_t generation = 1;

    window = mmap(NULL, WINDOW, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    snapshot = malloc(WINDOW);
    after_translated = malloc(WINDOW);
    if (window == MAP_FAILED || !snapshot || !after_translated) return 2;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    if (pw_vm_posix_backend(&vm) != PW_OK ||
        pw_x86_engine_init(&engine, &vm, entries, ENTRIES, 1u << 22, generation, source_view, NULL) != PW_OK ||
        pw_x86_hostexec_init(&hx, &vm, 1u << 24) != PW_OK)
        return 2;
    /* "global": the guest GPRs in fixed host registers, as wowprospero runs. */
    if (argc > 1 && !strcmp(argv[1], "global") &&
        pw_x86_engine_set_global_resident(&engine, 0xfb) != PW_OK)
        return 2;

    while (fgets(line, sizeof(line), stdin)) {
        uint8_t code[15];
        size_t len;
        PwX86HostExecPlan plan;
        PwX86State initial;
        const char *first = NULL;
        unsigned both_ran = 0;

        if (!parse_hex(line, code, &len)) continue;
        random_state(&initial, code, len);
        {
            uint8_t out[8192];
            PwX86Block block;
            if (pw_x86_translate(code, len, initial.eip, out, sizeof(out), &block) != PW_OK ||
                block.source_bytes != len || pw_x86_hostexec_plan(&initial, code, len, &plan) != PW_OK ||
                plan.length != len) {
                skipped++;
                continue;
            }
        }
        for (unsigned trial = 0; trial < TRIALS && !first; trial++) {
            PwX86State t, h;
            int ft, fh, st = PW_OK, sh = PW_OK;

            random_state(&initial, code, len);
            for (size_t i = 0x1000; i < WINDOW - 0x2000; i += 4) {
                uint32_t v = next_random();
                memcpy(window + i, &v, 4);
            }
            memcpy(window + CODE_AT, code, len);
            if (extra) { munmap(extra, 0x2000); extra = NULL; }
            if (pw_x86_hostexec_plan(&initial, code, len, &plan) == PW_OK && plan.mem_form &&
                (plan.address < initial.memory[0].low || plan.address + 64 > initial.memory[0].high)) {
                uintptr_t page = plan.address & ~(uintptr_t)0xfff;
                void *m = page >= 0x10000 ? mmap((void *)page, 0x2000, PROT_READ | PROT_WRITE,
                                                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0)
                                          : MAP_FAILED;
                if (m == (void *)page) {
                    extra = m;
                    for (size_t i = 0; i < 0x2000; i += 4) { uint32_t v = next_random(); memcpy(extra + i, &v, 4); }
                    memcpy(extra_snapshot, extra, 0x2000);
                    initial.memory_count = 2;
                    initial.memory[1].low = page;
                    initial.memory[1].high = (uint64_t)page + 0x2000;
                    initial.memory[1].permissions = PW_X86_READ | PW_X86_WRITE;
                } else if (m != MAP_FAILED) munmap(m, 0x2000);
            }
            memcpy(snapshot, window, WINDOW);
            current_code = window + CODE_AT;
            current_len = len;

            t = initial;
            pw_x86_engine_reset(&engine, ++generation);
            ft = run_translated(&engine, &t, &st);
            if (!ft) pw_x86_commit_canonical_flags(&t);
            memcpy(after_translated, window, WINDOW);
            if (extra) { memcpy(extra_translated, extra, 0x2000); memcpy(extra, extra_snapshot, 0x2000); }

            memcpy(window, snapshot, WINDOW);
            h = initial;
            fh = run_host(&hx, &h, len, &sh);

            if (ft || fh || st != PW_OK || sh != PW_OK) {
                faults++;
                if ((ft || st != PW_OK) != (fh || sh != PW_OK) && st != PW_ERR_VM) {
                    static char what[64];
                    snprintf(what, sizeof(what), "fault translated=%d/%d host=%d/%d", ft, st, fh, sh);
                    first = what;
                }
                continue;
            }
            both_ran++;
            first = compare(&t, &h, undefined_flags(code, len));
        }
        tested++;
        if (first) {
            mismatched++;
            printf("MISMATCH ");
            for (size_t i = 0; i < len; i++) printf("%02x", code[i]);
            printf(" %s\n", first);
            fflush(stdout);
        }
        (void)both_ran;
    }
    printf("differential: tested %llu skipped %llu mismatched %llu fault-trials %llu\n",
           (unsigned long long)tested, (unsigned long long)skipped,
           (unsigned long long)mismatched, (unsigned long long)faults);
    return mismatched ? 1 : 0;
}
