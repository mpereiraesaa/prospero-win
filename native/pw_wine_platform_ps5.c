/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_wine_platform_ps5.h"
#include "../include/prospero_win.h"

#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stddef.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
/* sys/event.h uses the BSD u_short/u_int names, which strict -std=c11 hides. */
typedef unsigned short u_short;
typedef unsigned int u_int;
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/ucontext.h>
#include <unistd.h>

/* FreeBSD amd64 sysarch operations (machine/sysarch.h). */
enum { PW_AMD64_GET_GSBASE = 130, PW_AMD64_SET_GSBASE = 131 };
int sysarch(int number, void *args);

enum { MALLOC_STEP = 1u << 20, MALLOC_STEPS = 256 };

static uint64_t read_gs_teb(void)
{
    uint64_t value;
    __asm__ volatile("movq %%gs:0x30, %0" : "=r"(value));
    return value;
}

/* ---- GS base ------------------------------------------------------------ */

typedef struct GsThread { uint64_t fake[8]; int set_rc, read_ok; } GsThread;

static void *gs_thread(void *opaque)
{
    GsThread *t = opaque;
    uint64_t original = 0, base = (uint64_t)(uintptr_t)t->fake;

    t->fake[6] = base;   /* offset 0x30 holds the TEB self pointer */
    (void)sysarch(PW_AMD64_GET_GSBASE, &original);
    t->set_rc = sysarch(PW_AMD64_SET_GSBASE, &base);
    if (t->set_rc == 0) {
        t->read_ok = read_gs_teb() == base;
        (void)sysarch(PW_AMD64_SET_GSBASE, &original);
    }
    return NULL;
}

void pw_wine_platform_ps5_gsbase(PwWinePlatformReport *r)
{
    static uint64_t fake[8];
    uint64_t original = 0, base = (uint64_t)(uintptr_t)fake;
    GsThread other;
    pthread_t thread;

    fake[6] = base;
    (void)sysarch(PW_AMD64_GET_GSBASE, &original);
    errno = 0;
    r->gsbase_set_rc = sysarch(PW_AMD64_SET_GSBASE, &base);
    r->gsbase_errno = r->gsbase_set_rc ? errno : 0;
    if (r->gsbase_set_rc != 0) return;
    r->gsbase_read_ok = read_gs_teb() == base;
    memset(&other, 0, sizeof(other));
    if (pthread_create(&thread, NULL, gs_thread, &other) == 0) {
        pthread_join(thread, NULL);
        /* The other thread's base must not have replaced this one. */
        r->gsbase_thread_ok = other.set_rc == 0 && other.read_ok && read_gs_teb() == base;
    }
    (void)sysarch(PW_AMD64_SET_GSBASE, &original);
}

/* ---- SIGSEGV on an alternate stack ------------------------------------- */

static uint8_t *altstack_low, *altstack_high;
static volatile int segv_hits, segv_on_altstack, segv_found_offset, segv_edit_measured;
static volatile uintptr_t segv_expected_ip;
static sigjmp_buf segv_escape;
extern const uint8_t pw_wine_probe_fault1[], pw_wine_probe_fault2[];

/* The saved RIP is located by value (the faulting instruction's address), so
 * the probe does not trust the SDK's ucontext layout. A trial edits either
 * the SDK's mc_rip field or the measured slot to skip the 3-byte load; if the
 * fault repeats, the edit had no effect and siglongjmp ends the trial. */
static void segv_handler(int sig, siginfo_t *info, void *opaque)
{
    ucontext_t *uc = opaque;
    uint8_t *bytes = (uint8_t *)uc, marker;

    (void)sig; (void)info;
    segv_hits++;
    segv_on_altstack = &marker >= altstack_low && &marker < altstack_high;
    if (segv_hits > 1) siglongjmp(segv_escape, 1);
    segv_found_offset = -1;
    for (int off = 0; off + 8 <= (int)sizeof(*uc); off += 8) {
        uint64_t v;
        memcpy(&v, bytes + off, 8);
        if (v == segv_expected_ip) { segv_found_offset = off; break; }
    }
    if (segv_edit_measured) {
        uint64_t v;
        if (segv_found_offset < 0) siglongjmp(segv_escape, 1);
        memcpy(&v, bytes + segv_found_offset, 8);
        v += 3;
        memcpy(bytes + segv_found_offset, &v, 8);
    } else
        uc->uc_mcontext.mc_rip += 3;
}

void pw_wine_platform_ps5_sigsegv(PwWinePlatformReport *r)
{
    static uint8_t stack[64 * 1024];
    struct sigaction action, previous, previous_bus;
    stack_t alt = { .ss_sp = stack, .ss_size = sizeof(stack), .ss_flags = 0 }, old_alt;

    altstack_low = stack;
    altstack_high = stack + sizeof(stack);
    r->sigsegv_header_offset = (int)offsetof(ucontext_t, uc_mcontext.mc_rip);
    r->sigsegv_rip_offset = -1;
    if (sigaltstack(&alt, &old_alt) != 0) { r->sigsegv_rc = -errno; return; }
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = segv_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGSEGV, &action, &previous) != 0 ||
        sigaction(SIGBUS, &action, &previous_bus) != 0) {
        r->sigsegv_rc = -errno;
        (void)sigaltstack(&old_alt, NULL);
        return;
    }

    /* Trial 1: edit the SDK header's mc_rip. */
    segv_hits = 0; segv_edit_measured = 0;
    segv_expected_ip = (uintptr_t)pw_wine_probe_fault1;
    if (sigsetjmp(segv_escape, 1) == 0) {
        uint64_t address = 8;   /* unmapped page-zero address */
        __asm__ volatile(".globl pw_wine_probe_fault1\npw_wine_probe_fault1:\n\t"
                         ".byte 0x48, 0x8b, 0x00" : "+a"(address) : : "memory");
        r->sigsegv_rip_edit_ok = segv_hits == 1;
    }
    r->sigsegv_hits = segv_hits;
    r->sigsegv_recovered = segv_hits >= 1;
    r->sigsegv_on_altstack = segv_on_altstack;
    r->sigsegv_rip_offset = segv_found_offset;

    /* Trial 2: edit the measured slot. */
    if (r->sigsegv_rip_offset >= 0) {
        segv_hits = 0; segv_edit_measured = 1;
        segv_expected_ip = (uintptr_t)pw_wine_probe_fault2;
        if (sigsetjmp(segv_escape, 1) == 0) {
            uint64_t address = 8;
            __asm__ volatile(".globl pw_wine_probe_fault2\npw_wine_probe_fault2:\n\t"
                             ".byte 0x48, 0x8b, 0x00" : "+a"(address) : : "memory");
            r->sigsegv_measured_edit_ok = segv_hits == 1;
        }
    }
    (void)sigaction(SIGSEGV, &previous, NULL);
    (void)sigaction(SIGBUS, &previous_bus, NULL);
    (void)sigaltstack(&old_alt, NULL);
}

/* ---- socketpair + SCM_RIGHTS, kqueue ------------------------------------ */

void pw_wine_platform_ps5_sockets(PwWinePlatformReport *r)
{
    int sv[2], pipefd[2], received = -1, kq;
    char byte = 'w', got = 0;
    char control[CMSG_SPACE(sizeof(int))];
    struct iovec iov = { &byte, 1 };
    struct msghdr msg;
    struct cmsghdr *cmsg;
    struct kevent change, event;
    struct timespec timeout = { 0, 200 * 1000 * 1000 };

    errno = 0;
    r->socketpair_rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    if (r->socketpair_rc != 0) { r->socket_errno = errno; return; }
    if (pipe(pipefd) == 0) {
        memset(&msg, 0, sizeof(msg));
        memset(control, 0, sizeof(control));
        msg.msg_iov = &iov; msg.msg_iovlen = 1;
        msg.msg_control = control; msg.msg_controllen = sizeof(control);
        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &pipefd[0], sizeof(int));
        if (sendmsg(sv[0], &msg, 0) == 1) {
            memset(control, 0, sizeof(control));
            msg.msg_controllen = sizeof(control);
            if (recvmsg(sv[1], &msg, 0) == 1 && (cmsg = CMSG_FIRSTHDR(&msg)) &&
                cmsg->cmsg_type == SCM_RIGHTS) {
                memcpy(&received, CMSG_DATA(cmsg), sizeof(int));
                if (write(pipefd[1], "x", 1) == 1 && read(received, &got, 1) == 1)
                    r->scm_rights_ok = got == 'x';
                close(received);
            }
        } else r->socket_errno = errno;
        close(pipefd[0]); close(pipefd[1]);
    }

    errno = 0;
    kq = kqueue();
    r->kqueue_rc = kq < 0 ? -1 : 0;
    if (kq < 0) { r->kqueue_errno = errno; goto done; }
    EV_SET(&change, sv[1], EVFILT_READ, EV_ADD, 0, 0, NULL);
    if (kevent(kq, &change, 1, NULL, 0, NULL) == 0 && write(sv[0], "k", 1) == 1)
        r->kevent_ready = kevent(kq, NULL, 0, &event, 1, &timeout) == 1 &&
                          (int)event.ident == sv[1];
    else r->kqueue_errno = errno;
    close(kq);
done:
    close(sv[0]); close(sv[1]);
}

/* ---- malloc ceiling ----------------------------------------------------- */

void pw_wine_platform_ps5_malloc(PwWinePlatformReport *r)
{
    static void *blocks[MALLOC_STEPS];
    unsigned count = 0;

    r->malloc_limit_bytes = (uint64_t)MALLOC_STEPS * MALLOC_STEP;
    while (count < MALLOC_STEPS && (blocks[count] = malloc(MALLOC_STEP)) != NULL) {
        ((volatile uint8_t *)blocks[count])[0] = 1;
        ((volatile uint8_t *)blocks[count])[MALLOC_STEP - 1] = 1;
        count++;
    }
    r->malloc_ceiling_bytes = (uint64_t)count * MALLOC_STEP;
    while (count) free(blocks[--count]);
}

int pw_wine_platform_ps5_gsbase_get(uint64_t *value, int *os_errno)
{
    int rc;

    errno = 0;
    *value = 0;
    rc = sysarch(PW_AMD64_GET_GSBASE, value);
    *os_errno = rc ? errno : 0;
    return rc;
}

int pw_wine_platform_ps5_gsbase_same(int *os_errno)
{
    uint64_t value = 0;
    int rc;

    if ((rc = pw_wine_platform_ps5_gsbase_get(&value, os_errno)) != 0) return rc;
    errno = 0;
    rc = sysarch(PW_AMD64_SET_GSBASE, &value);
    *os_errno = rc ? errno : 0;
    return rc;
}
