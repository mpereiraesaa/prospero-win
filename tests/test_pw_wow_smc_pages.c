/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The write-protection state of wowprospero's writable code pages
 * (smc_pages.h), with a recorded mprotect and a controlled clock;
 * test_pw_x86_smc runs it against real faults. */
#define _GNU_SOURCE
#include "../wine/wowprospero/smc_pages.h"
#include <assert.h>
#include <stdio.h>

enum { HOST = 0x4000 };
static struct { uintptr_t address; size_t bytes; int prot; } calls[64];
static unsigned call_count, refuse;
static uint64_t clock_ms;

static int record(void *address, size_t bytes, int prot)
{
    assert(call_count < 64);
    calls[call_count].address = (uintptr_t)address;
    calls[call_count].bytes = bytes;
    calls[call_count++].prot = prot;
    return refuse ? -1 : 0;
}

static uint64_t now(void) { return clock_ms; }

static const PwSmcOps ops = { record, now };
static PwSmcPages smc;

int main(void)
{
    const int rwx = PROT_READ | PROT_WRITE | PROT_EXEC;
    const uint32_t code = 0x00401234, other = 0x00405000;

    assert(pw_smc_init(&smc, 0x3000, &ops) == -1);
    assert(!pw_smc_enabled(&smc) && !pw_smc_protect(&smc, code, rwx) && !pw_smc_fault(&smc, code));
    assert(!pw_smc_init(&smc, HOST, &ops));

    /* Protecting takes write permission from the whole host page, once. */
    assert(!pw_smc_protect(&smc, code, PROT_READ | PROT_EXEC));  /* nothing to protect */
    assert(pw_smc_protect(&smc, code, rwx) && call_count == 1);
    assert(calls[0].address == 0x400000 && calls[0].bytes == HOST && calls[0].prot == (PROT_READ | PROT_EXEC));
    assert(pw_smc_protect(&smc, 0x403fff, rwx) && call_count == 1 && smc.protects == 1);
    assert(pw_smc_state(&smc, 0x400000) == PW_SMC_PROTECTED && pw_smc_state(&smc, other) == PW_SMC_NONE);

    /* A fault elsewhere is not ours; one on the page gives its permission
     * back and leaves it unprotected until code is translated from it. */
    assert(!pw_smc_fault(&smc, other) && !pw_smc_fault(&smc, 0x100000000ull));
    clock_ms = 1000;
    assert(pw_smc_fault(&smc, code + 8) && call_count == 2 && calls[1].prot == rwx);
    assert(pw_smc_state(&smc, code) == PW_SMC_NONE && smc.faults == 1);
    /* A fault on it now is Wine's. */
    assert(!pw_smc_fault(&smc, code));

    /* Writes far apart are forgiven: the page stays on protection. */
    for (unsigned k = 0; k < 10; k++) {
        clock_ms += PW_SMC_DECAY_MS + 1;
        assert(pw_smc_protect(&smc, code, rwx) && pw_smc_fault(&smc, code));
    }
    assert(pw_smc_state(&smc, code) == PW_SMC_NONE && !smc.demotions);
    /* A burst sends it to the checks for good. */
    for (unsigned k = 1; k < PW_SMC_DEMOTE_FAULTS; k++) {
        clock_ms += 10;
        assert(pw_smc_protect(&smc, code, rwx) && pw_smc_fault(&smc, code));
    }
    assert(pw_smc_state(&smc, code) == PW_SMC_CHECKED && smc.demotions == 1);
    assert(!pw_smc_protect(&smc, code, rwx));
    /* A notification that it is still committed keeps it there; a free
     * starts it afresh. */
    pw_smc_refresh(&smc, code, 1, 1, rwx);
    assert(pw_smc_state(&smc, code) == PW_SMC_CHECKED);
    pw_smc_refresh(&smc, code, 0, 0, 0);
    assert(pw_smc_state(&smc, code) == PW_SMC_NONE);

    /* A protected page that still qualifies after a notification is
     * protected again, with the protection Wine gives it now; one that no
     * longer does is forgotten, its host protection left to Wine. */
    call_count = 0;
    assert(pw_smc_protect(&smc, other, rwx) && call_count == 1);
    pw_smc_refresh(&smc, other, 1, 1, PROT_READ | PROT_WRITE);
    assert(call_count == 2 && calls[1].prot == PROT_READ && pw_smc_state(&smc, other) == PW_SMC_PROTECTED);
    pw_smc_refresh(&smc, other, 1, 1, PROT_READ | PROT_EXEC);
    assert(call_count == 2 && pw_smc_state(&smc, other) == PW_SMC_NONE);

    /* A page that does not qualify is passed over until a notification. */
    pw_smc_ineligible(&smc, other);
    assert(pw_smc_state(&smc, other) == PW_SMC_INELIGIBLE && !pw_smc_protect(&smc, other, rwx));
    pw_smc_refresh(&smc, other, 1, 1, rwx);
    assert(pw_smc_state(&smc, other) == PW_SMC_NONE);

    /* An mprotect the host refuses leaves the page to the checks. */
    refuse = 1;
    assert(!pw_smc_protect(&smc, 0x500000, rwx) && pw_smc_state(&smc, 0x500000) == PW_SMC_INELIGIBLE);
    refuse = 0;

    /* After a protection change that succeeded: a page at the checks stays
     * there and needs nothing; one that did not qualify is NONE again, with
     * no host call; NONE and PROTECTED pages need the full refresh. */
    {
        const unsigned before = call_count;
        const uint32_t checked = 0x600000, ineligible = 0x700000;

        for (unsigned k = 0; k < PW_SMC_DEMOTE_FAULTS; k++) {
            clock_ms += 10;
            assert(pw_smc_protect(&smc, checked, rwx) && pw_smc_fault(&smc, checked));
        }
        assert(pw_smc_state(&smc, checked) == PW_SMC_CHECKED);
        assert(!pw_smc_after_protect(&smc, checked + 0x123) && pw_smc_state(&smc, checked) == PW_SMC_CHECKED);
        pw_smc_ineligible(&smc, ineligible);
        assert(!pw_smc_after_protect(&smc, ineligible) && pw_smc_state(&smc, ineligible) == PW_SMC_NONE);
        assert(pw_smc_after_protect(&smc, ineligible));                 /* NONE */
        assert(pw_smc_protect(&smc, ineligible, rwx));
        assert(pw_smc_after_protect(&smc, ineligible));                 /* PROTECTED */
        assert(!pw_smc_after_protect(&smc, 0x100000000ull));
        assert(call_count == before + 2 * PW_SMC_DEMOTE_FAULTS + 1);   /* only the protects and faults */
        pw_smc_refresh(&smc, ineligible, 0, 0, 0);
        pw_smc_refresh(&smc, checked, 0, 0, 0);
    }

    /* Hooking code that makes the same code writable and read-only around
     * every patch (Proper Shaders in San Andreas, every frame): each time,
     * the page is protected while writable and given up when it goes
     * read-only. A burst of that sends it to the checks; spread out, it is
     * forgiven. */
    {
        const uint32_t toggled = 0x800000, refused = 0x900000;
        const uint64_t demotions = smc.demotions;

        for (unsigned k = 0; k < 3; k++) {
            clock_ms += PW_SMC_DECAY_MS + 1;
            assert(pw_smc_protect(&smc, toggled, rwx));
            pw_smc_refresh(&smc, toggled, 1, 0, PROT_READ | PROT_EXEC);
            assert(pw_smc_state(&smc, toggled) == PW_SMC_NONE);
        }
        for (unsigned k = 1; k < PW_SMC_DEMOTE_FAULTS; k++) {
            clock_ms += 5;
            assert(pw_smc_protect(&smc, toggled, rwx));
            pw_smc_refresh(&smc, toggled, 1, 0, PROT_READ | PROT_EXEC);
        }
        assert(pw_smc_state(&smc, toggled) == PW_SMC_CHECKED && smc.demotions == demotions + 1);
        assert(!pw_smc_after_protect(&smc, toggled));

        /* A host that refuses the protection every time: INELIGIBLE, back to
         * NONE at the next protection change, refused again... to the checks. */
        refuse = 1;
        for (unsigned k = 1; k < PW_SMC_DEMOTE_FAULTS; k++) {
            clock_ms += 5;
            assert(!pw_smc_protect(&smc, refused, rwx) && pw_smc_state(&smc, refused) == PW_SMC_INELIGIBLE);
            assert(!pw_smc_after_protect(&smc, refused) && pw_smc_state(&smc, refused) == PW_SMC_NONE);
        }
        assert(!pw_smc_protect(&smc, refused, rwx) && pw_smc_state(&smc, refused) == PW_SMC_CHECKED);
        assert(smc.demotions == demotions + 2);
        refuse = 0;
        pw_smc_refresh(&smc, toggled, 0, 0, 0);
        pw_smc_refresh(&smc, refused, 0, 0, 0);
    }

    /* Pages are remembered for notifications of unknown extent while they
     * need it: each page here took over the entry of one that no longer
     * did. */
    assert(smc.tracked_count <= 5 && smc.tracked[0] == 0x500000 / HOST);
    pw_smc_destroy(&smc);
    printf("wowprospero write-protected code pages passed: protect, fault, decay, demotion, "
           "notifications, refused mprotect, protection changes, toggled pages\n");
    return 0;
}
