/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Measure where the console's signal ucontext stores each general register.
 *
 * Every GPR except RSP is loaded with a distinct non-canonical sentinel and
 * the next instruction dereferences RAX, so the fault is taken with all of
 * them live. The handler, running on an alternate stack, searches the
 * ucontext for each sentinel (and for the recorded RSP and the faulting RIP)
 * and leaves with siglongjmp, which restores every callee-saved register the
 * probe overwrote. The SDK header's layout is reported but never trusted.
 */
#include "pw_ucontext_map_ps5.h"
#include "../include/prospero_win.h"

#include <setjmp.h>
#include <signal.h>
#include <stddef.h>
#include <string.h>
#include <sys/ucontext.h>

#define SENTINEL(index) (0xdead5ec000000000ull | ((uint64_t)(index) << 8) | 0x5aull)

static uint8_t map_stack[64 * 1024] __attribute__((aligned(16)));
static sigjmp_buf map_escape;
static volatile uint64_t map_rsp_at_fault;
static PwUcontextMap *volatile map_out;
extern const uint8_t pw_ucontext_map_fault[];

static uint64_t expected_value(unsigned index)
{
    if (index == PW_UC_RSP) return map_rsp_at_fault;
    if (index == PW_UC_RIP) return (uint64_t)(uintptr_t)pw_ucontext_map_fault;
    return SENTINEL(index);
}

static void map_handler(int sig, siginfo_t *info, void *opaque)
{
    const uint8_t *uc = opaque;
    PwUcontextMap *map = map_out;
    size_t limit = 1024;

    (void)info;
    if ((const uint8_t *)uc >= map_stack && (const uint8_t *)uc < map_stack + sizeof(map_stack))
        limit = (size_t)(map_stack + sizeof(map_stack) - uc);
    if (limit > 2048) limit = 2048;
    map->scanned = (int)limit;
    map->signal = sig;
    for (unsigned r = 0; r < PW_UC_COUNT; r++) {
        const uint64_t want = expected_value(r);
        map->offset[r] = -1;
        for (size_t off = 0; off + 8 <= limit; off += 8) {
            uint64_t v;
            memcpy(&v, uc + off, 8);
            if (v != want) continue;
            if (map->offset[r] < 0) map->offset[r] = (int)off;
            else map->duplicates++;
        }
    }
    map->rc = 0;
    siglongjmp(map_escape, 1);
}

/* Loads the sentinels and faults; never returns normally. */
static void __attribute__((noinline, noreturn)) map_fault(void)
{
    __asm__ volatile(
        "movq %%rsp, %[rsp]\n\t"
        "movabsq %[s1], %%rbx\n\t"
        "movabsq %[s2], %%rcx\n\t"
        "movabsq %[s3], %%rdx\n\t"
        "movabsq %[s4], %%rsi\n\t"
        "movabsq %[s5], %%rdi\n\t"
        "movabsq %[s6], %%rbp\n\t"
        "movabsq %[s7], %%r8\n\t"
        "movabsq %[s8], %%r9\n\t"
        "movabsq %[s9], %%r10\n\t"
        "movabsq %[s10], %%r11\n\t"
        "movabsq %[s11], %%r12\n\t"
        "movabsq %[s12], %%r13\n\t"
        "movabsq %[s13], %%r14\n\t"
        "movabsq %[s14], %%r15\n\t"
        "movabsq %[s0], %%rax\n\t"
        ".globl pw_ucontext_map_fault\n"
        "pw_ucontext_map_fault:\n\t"
        "movq (%%rax), %%rax\n\t"
        "ud2\n"
        : [rsp] "=m"(map_rsp_at_fault)
        : [s0] "i"(SENTINEL(PW_UC_RAX)), [s1] "i"(SENTINEL(PW_UC_RBX)),
          [s2] "i"(SENTINEL(PW_UC_RCX)), [s3] "i"(SENTINEL(PW_UC_RDX)),
          [s4] "i"(SENTINEL(PW_UC_RSI)), [s5] "i"(SENTINEL(PW_UC_RDI)),
          [s6] "i"(SENTINEL(PW_UC_RBP)), [s7] "i"(SENTINEL(PW_UC_R8)),
          [s8] "i"(SENTINEL(PW_UC_R9)), [s9] "i"(SENTINEL(PW_UC_R10)),
          [s10] "i"(SENTINEL(PW_UC_R11)), [s11] "i"(SENTINEL(PW_UC_R12)),
          [s12] "i"(SENTINEL(PW_UC_R13)), [s13] "i"(SENTINEL(PW_UC_R14)),
          [s14] "i"(SENTINEL(PW_UC_R15))
        : "memory");
    __builtin_unreachable();
}

int pw_ucontext_map_ps5(PwUcontextMap *map)
{
    struct sigaction action, old_segv, old_bus;
    stack_t alt = { .ss_sp = map_stack, .ss_size = sizeof(map_stack), .ss_flags = 0 }, old_alt;

    if (!map) return PW_ERR_PRECONDITION;
    memset(map, 0, sizeof(*map));
    map->rc = -1;
    for (unsigned r = 0; r < PW_UC_COUNT; r++) map->offset[r] = -1;
    map->header_rip = (int)offsetof(ucontext_t, uc_mcontext.mc_rip);
    map->header_rsp = (int)offsetof(ucontext_t, uc_mcontext.mc_rsp);
    map->header_rax = (int)offsetof(ucontext_t, uc_mcontext.mc_rax);
    map->header_size = (int)sizeof(ucontext_t);
    map_out = map;
    if (sigaltstack(&alt, &old_alt) != 0) return PW_ERR_STATE;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = map_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGSEGV, &action, &old_segv) != 0 || sigaction(SIGBUS, &action, &old_bus) != 0) {
        (void)sigaltstack(&old_alt, NULL);
        return PW_ERR_STATE;
    }
    if (sigsetjmp(map_escape, 1) == 0) map_fault();
    (void)sigaction(SIGSEGV, &old_segv, NULL);
    (void)sigaction(SIGBUS, &old_bus, NULL);
    (void)sigaltstack(&old_alt, NULL);
    return map->rc == 0 ? PW_OK : PW_ERR_STATE;
}

/* ---- FSGSBASE ----------------------------------------------------------- */

static sigjmp_buf ill_escape;

static void ill_handler(int sig, siginfo_t *info, void *opaque)
{
    (void)sig; (void)info; (void)opaque;
    siglongjmp(ill_escape, 1);
}

void pw_fsgsbase_ps5_probe(PwFsGsBaseProbe *p)
{
    struct sigaction action, old_ill, old_segv, old_bus;
    stack_t alt = { .ss_sp = map_stack, .ss_size = sizeof(map_stack), .ss_flags = 0 }, old_alt;
    volatile uint64_t value = 0;

    memset(p, 0, sizeof(*p));
    if (sigaltstack(&alt, &old_alt) != 0) return;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = ill_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    sigaction(SIGILL, &action, &old_ill);
    sigaction(SIGSEGV, &action, &old_segv);
    sigaction(SIGBUS, &action, &old_bus);
    if (sigsetjmp(ill_escape, 1) == 0) {
        uint64_t v;
        __asm__ volatile("rdfsbase %0" : "=r"(v));
        value = v; p->fs_value = value; p->rdfsbase = 1;
    }
    if (sigsetjmp(ill_escape, 1) == 0) {
        uint64_t v;
        __asm__ volatile("rdgsbase %0" : "=r"(v));
        value = v; p->gs_value = value; p->rdgsbase = 1;
    }
    if (p->rdgsbase && sigsetjmp(ill_escape, 1) == 0) {
        uint64_t same = p->gs_value;
        __asm__ volatile("wrgsbase %0" : : "r"(same) : "memory");
        p->wrgsbase = 1;
    }
    sigaction(SIGILL, &old_ill, NULL);
    sigaction(SIGSEGV, &old_segv, NULL);
    sigaction(SIGBUS, &old_bus, NULL);
    (void)sigaltstack(&old_alt, NULL);
}
