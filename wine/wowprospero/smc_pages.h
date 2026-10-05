/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Header-only: the Unix side of the backend and its unit tests include it. */
#ifndef PW_SMC_PAGES_H
#define PW_SMC_PAGES_H
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>

/* Code on writable guest pages, write-protected on the host instead of
 * checked on every entry (the approach of FEX, box64 and QEMU user mode).
 *
 * A translation read from a writable page normally compares its source with
 * a copy whenever it is entered (PwX86SourceViewWritable). When the page
 * qualifies, the backend instead takes write permission away from its host
 * page, while Wine's own view of it stays writable, and translates it as if
 * it were read-only: its blocks run with no check at all. A guest write to
 * the page then faults. The fault handler gives the host page its
 * permission back, has every thread discard its translations (the caller
 * does that) and resumes the write; the page is protected again the next
 * time code is translated from it.
 *
 * A page that faults too often, such as Mod Loader's CText::Get toggled
 * around every call it passes on, or code that shares its host page with
 * data written all the time, goes back to the checks for good (CHECKED).
 *
 * State is kept per host page (16 KiB on the PS5, so one protection covers
 * four 4 KiB guest pages). The fault handler takes the lock; everything else
 * takes it with every signal blocked, so a thread that holds it can never
 * be stopped (suspended by Wine, say) while another waits in the handler.
 * Nothing that holds the lock writes guest memory or calls into Wine. */
enum {
    PW_SMC_NONE = 0,        /* not protected; may be protected */
    PW_SMC_PROTECTED = 1,   /* write-protected on the host by us */
    PW_SMC_CHECKED = 2,     /* faulted too often: translations check it */
    PW_SMC_INELIGIBLE = 3,  /* does not qualify until its protection changes */
};

/* Faults that send a page back to the checks, and how fast its count
 * decays: one fault forgiven per PW_SMC_DECAY_MS. A page written once, at
 * startup or every few seconds, stays protected; one written in a burst or
 * steadily goes to the checks after a handful of global flushes. */
enum { PW_SMC_DEMOTE_FAULTS = 4, PW_SMC_DECAY_MS = 10000, PW_SMC_TRACKED = 16384 };

typedef struct PwSmcPage {
    uint8_t state;
    uint8_t prot;      /* the host protection Wine gave it (PROT_*) */
    uint8_t faults;
    uint8_t reserved;
    uint32_t last_ms;  /* the last fault, from now_ms */
} PwSmcPage;

typedef struct PwSmcOps {
    /* The host mprotect (on the PS5 the one Wine's own calls go through). */
    int (*protect)(void *address, size_t bytes, int prot);
    /* A monotonic clock in milliseconds; called from the fault handler. */
    uint64_t (*now_ms)(void);
} PwSmcOps;

typedef struct PwSmcPages {
    PwSmcPage *pages;          /* one per host page below 4 GiB, or NULL: off */
    uintptr_t host_page;
    unsigned shift;
    volatile int lock;
    /* The host pages ever protected, for notifications of unknown extent;
     * entries stay after their page leaves PROTECTED and are reused. */
    uint32_t tracked[PW_SMC_TRACKED];
    volatile uint32_t tracked_count;
    PwSmcOps ops;
    /* Totals: pages protected, faults resolved, of them on pages with
     * translations (each a global flush), and pages sent to the checks. */
    volatile uint64_t protects, faults, flushes, demotions;
} PwSmcPages;

/* Set up for host pages of host_page bytes (a power of two, at least 4 KiB);
 * 0, or -1 with the pages left off. */
static inline int pw_smc_init(PwSmcPages *smc, size_t host_page, const PwSmcOps *ops)
{
    unsigned shift = 0;

    if (!smc || !ops || !ops->protect || !ops->now_ms || host_page < 0x1000 || (host_page & (host_page - 1)))
        return -1;
    while (((size_t)1 << shift) < host_page) shift++;
    smc->pages = calloc((size_t)1 << (32 - shift), sizeof(*smc->pages));
    if (!smc->pages) return -1;
    smc->host_page = host_page;
    smc->shift = shift;
    smc->lock = 0;
    smc->tracked_count = 0;
    smc->ops = *ops;
    smc->protects = smc->faults = smc->flushes = smc->demotions = 0;
    return 0;
}

static inline void pw_smc_destroy(PwSmcPages *smc)
{
    free(smc->pages);
    smc->pages = NULL;
}

static inline int pw_smc_enabled(const PwSmcPages *smc)
{
    return smc && smc->pages != NULL;
}

/* The host page holding guest address, as an index; address < 4 GiB. */
static inline uint32_t pw_smc_index(const PwSmcPages *smc, uint64_t address)
{
    return (uint32_t)(address >> smc->shift);
}

static inline uintptr_t pw_smc_base(const PwSmcPages *smc, uint32_t index)
{
    return (uintptr_t)index << smc->shift;
}

static inline int pw_smc_state(const PwSmcPages *smc, uint64_t address)
{
    if (!pw_smc_enabled(smc) || address >= 0x100000000ull) return PW_SMC_NONE;
    return __atomic_load_n(&smc->pages[pw_smc_index(smc, address)].state, __ATOMIC_ACQUIRE);
}

static inline void pw_smc_spin(PwSmcPages *smc)
{
    while (__atomic_exchange_n(&smc->lock, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&smc->lock, __ATOMIC_RELAXED)) __builtin_ia32_pause();
}

static inline void pw_smc_unspin(PwSmcPages *smc)
{
    __atomic_store_n(&smc->lock, 0, __ATOMIC_RELEASE);
}

/* The lock outside the fault handler: every signal blocked while held. */
static inline void pw_smc_enter(PwSmcPages *smc, sigset_t *saved)
{
    sigset_t all;

    sigfillset(&all);
    sigprocmask(SIG_BLOCK, &all, saved);
    pw_smc_spin(smc);
}

static inline void pw_smc_leave(PwSmcPages *smc, const sigset_t *saved)
{
    pw_smc_unspin(smc);
    sigprocmask(SIG_SETMASK, saved, NULL);
}

/* Remember index among the tracked pages; 1 when it is, 0 when there is no
 * room. Under the lock. */
static inline int pw_smc_track(PwSmcPages *smc, uint32_t index)
{
    uint32_t count = smc->tracked_count, reuse = count;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t other = smc->tracked[i];
        if (other == index) return 1;
        /* The entry of a page that no longer needs one. */
        if (reuse == count && __atomic_load_n(&smc->pages[other].state, __ATOMIC_RELAXED) == PW_SMC_NONE)
            reuse = i;
    }
    if (reuse < count) smc->tracked[reuse] = index;
    else if (count < PW_SMC_TRACKED) {
        smc->tracked[count] = index;
        __atomic_store_n(&smc->tracked_count, count + 1, __ATOMIC_RELEASE);
    }
    else return 0;
    return 1;
}

/* One more strike against a page under the lock: a fault, or a protection
 * that did not hold (refused, or given up at the next notification). One
 * strike is forgiven per PW_SMC_DECAY_MS. 1 when the page has had enough
 * and goes to the checks, which the caller records. */
static inline int pw_smc_strike(PwSmcPages *smc, PwSmcPage *page)
{
    uint64_t now = smc->ops.now_ms();
    uint32_t elapsed = (uint32_t)now - page->last_ms, forgiven = elapsed / PW_SMC_DECAY_MS;

    page->faults = page->faults > forgiven ? (uint8_t)(page->faults - forgiven) : 0;
    if (page->faults < 255) page->faults++;
    page->last_ms = (uint32_t)now;
    return page->faults >= PW_SMC_DEMOTE_FAULTS;
}

static inline void pw_smc_demote(PwSmcPages *smc, PwSmcPage *page)
{
    __atomic_store_n(&page->state, PW_SMC_CHECKED, __ATOMIC_RELEASE);
    __atomic_add_fetch(&smc->demotions, 1, __ATOMIC_RELAXED);
}

/* Write-protect the host page holding address, which qualifies (the caller
 * checked) and whose host protection is prot (PROT_* with PROT_WRITE). 1
 * when it is protected now, or already was; 0 when it may not be, and then
 * translations read from it check their source. The caller has marked the
 * pages translations will read from first, so a write that faults from now
 * on finds them marked. */
static inline int pw_smc_protect(PwSmcPages *smc, uint64_t address, int prot)
{
    sigset_t saved;
    uint32_t index;
    PwSmcPage *page;
    int result = 0;

    if (!pw_smc_enabled(smc) || address >= 0x100000000ull || !(prot & PROT_WRITE)) return 0;
    index = pw_smc_index(smc, address);
    page = &smc->pages[index];
    switch (__atomic_load_n(&page->state, __ATOMIC_ACQUIRE)) {
    case PW_SMC_PROTECTED: return 1;
    case PW_SMC_NONE: break;
    default: return 0;
    }
    pw_smc_enter(smc, &saved);
    if (page->state == PW_SMC_PROTECTED) result = 1;
    else if (page->state == PW_SMC_NONE && pw_smc_track(smc, index)) {
        page->prot = (uint8_t)prot;
        if (!smc->ops.protect((void *)pw_smc_base(smc, index), smc->host_page, prot & ~PROT_WRITE)) {
            __atomic_store_n(&page->state, PW_SMC_PROTECTED, __ATOMIC_RELEASE);
            smc->protects++;
            result = 1;
        } else if (pw_smc_strike(smc, page)) {
            pw_smc_demote(smc, page);  /* refused again and again */
        } else {
            __atomic_store_n(&page->state, PW_SMC_INELIGIBLE, __ATOMIC_RELEASE);
        }
    }
    pw_smc_leave(smc, &saved);
    return result;
}

/* Give the host page its permission back. Under the lock. */
static inline void pw_smc_restore(PwSmcPages *smc, uint32_t index)
{
    void *base = (void *)pw_smc_base(smc, index);
    const int prot = smc->pages[index].prot;

    /* Wine's mprotect may have been refused execute permission and fallen
     * back to read-write; the guest's code never runs on the host anyway. */
    if (smc->ops.protect(base, smc->host_page, prot) && (prot & PROT_EXEC))
        (void)smc->ops.protect(base, smc->host_page, prot & ~PROT_EXEC);
}

/* From the SIGSEGV handler, before Wine sees the fault: a fault at address
 * on a host page we write-protected is the guest (or Wine, or another
 * thread) writing it. 1 when it was ours: the page is writable again, and
 * the faulting access may be resumed; 0 when Wine must handle the fault.
 * Whether translations were read from the page, and so must go, is the
 * caller's to check (code_pages.h) and act on before the access resumes. */
static inline int pw_smc_fault(PwSmcPages *smc, uint64_t address)
{
    uint32_t index;
    PwSmcPage *page;
    int demote;

    if (!pw_smc_enabled(smc) || address >= 0x100000000ull) return 0;
    index = pw_smc_index(smc, address);
    page = &smc->pages[index];
    if (__atomic_load_n(&page->state, __ATOMIC_ACQUIRE) != PW_SMC_PROTECTED) return 0;
    pw_smc_spin(smc);
    if (page->state != PW_SMC_PROTECTED) {
        /* Another thread resolved it first: try the access again. If the
         * page lost its protection some other way, the retry faults again
         * and that fault is Wine's. */
        pw_smc_unspin(smc);
        return 1;
    }
    demote = pw_smc_strike(smc, page);
    pw_smc_restore(smc, index);
    if (demote) {
        pw_smc_demote(smc, page);
    } else {
        __atomic_store_n(&page->state, PW_SMC_NONE, __ATOMIC_RELEASE);
    }
    __atomic_add_fetch(&smc->faults, 1, __ATOMIC_RELAXED);
    pw_smc_unspin(smc);
    return 1;
}

/* After a memory notification on the host page holding address (Wine has
 * applied the new protection, or freed it): committed and qualifies say
 * where it stands now, prot is its host protection when it qualifies. A
 * protected page that still qualifies is protected again, since Wine's
 * mprotect may have given write permission back; one that no longer does
 * is forgotten without touching its host protection, which is Wine's again.
 * A page that does not qualify may be considered again later; one that is
 * no longer committed starts afresh. */
static inline void pw_smc_refresh(PwSmcPages *smc, uint64_t address, int committed, int qualifies, int prot)
{
    sigset_t saved;
    uint32_t index;
    PwSmcPage *page;

    if (!pw_smc_enabled(smc) || address >= 0x100000000ull) return;
    index = pw_smc_index(smc, address);
    page = &smc->pages[index];
    if (__atomic_load_n(&page->state, __ATOMIC_ACQUIRE) == PW_SMC_NONE) return;
    pw_smc_enter(smc, &saved);
    switch (page->state) {
    case PW_SMC_PROTECTED:
        if (qualifies && (prot & PROT_WRITE)) {
            page->prot = (uint8_t)prot;
            if (smc->ops.protect((void *)pw_smc_base(smc, index), smc->host_page, prot & ~PROT_WRITE)) {
                pw_smc_restore(smc, index);
                __atomic_store_n(&page->state, PW_SMC_INELIGIBLE, __ATOMIC_RELEASE);
            }
        } else if (pw_smc_strike(smc, page)) {
            /* Protected and given up again and again: hooking code making
             * the same code writable and read-only around every patch. */
            pw_smc_demote(smc, page);
        } else {
            __atomic_store_n(&page->state, PW_SMC_NONE, __ATOMIC_RELEASE);
        }
        break;
    case PW_SMC_INELIGIBLE:
        __atomic_store_n(&page->state, PW_SMC_NONE, __ATOMIC_RELEASE);
        break;
    case PW_SMC_CHECKED:
        if (!committed) {
            page->faults = 0;
            __atomic_store_n(&page->state, PW_SMC_NONE, __ATOMIC_RELEASE);
        }
        break;
    }
    pw_smc_leave(smc, &saved);
}

/* After a protection change that succeeded on the host page holding
 * address, which leaves every page of it committed as it was: 1 when the
 * caller must refresh it as after any notification (pw_smc_refresh, from
 * its pages queried), 0 when it is settled here. A page at the checks
 * leaves them only when no longer committed, so it stays; a page that did
 * not qualify goes back to NONE whatever its protection. Hooking code that
 * patches the same game code every frame makes its pages CHECKED, and then
 * a protection change costs no query or signal mask. */
static inline int pw_smc_after_protect(PwSmcPages *smc, uint64_t address)
{
    if (!pw_smc_enabled(smc) || address >= 0x100000000ull) return 0;
    switch (pw_smc_state(smc, address)) {
    case PW_SMC_CHECKED:
        return 0;
    case PW_SMC_INELIGIBLE:
        pw_smc_refresh(smc, address, 1, 0, 0);
        return 0;
    default:
        return 1;
    }
}

/* Mark the host page holding address as not qualifying, until a memory
 * notification on it (pw_smc_refresh). */
static inline void pw_smc_ineligible(PwSmcPages *smc, uint64_t address)
{
    sigset_t saved;
    uint32_t index;

    if (!pw_smc_enabled(smc) || address >= 0x100000000ull) return;
    index = pw_smc_index(smc, address);
    if (__atomic_load_n(&smc->pages[index].state, __ATOMIC_ACQUIRE) != PW_SMC_NONE) return;
    pw_smc_enter(smc, &saved);
    if (smc->pages[index].state == PW_SMC_NONE && pw_smc_track(smc, index))
        __atomic_store_n(&smc->pages[index].state, PW_SMC_INELIGIBLE, __ATOMIC_RELEASE);
    pw_smc_leave(smc, &saved);
}
#endif
