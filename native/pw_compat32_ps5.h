/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * PS5 platform half of the compatibility-mode diagnostic.
 *
 * Descriptor installation goes through `sysarch(I386_SET_LDT, ...)`, which
 * FreeBSD amd64 implements as `amd64_set_ldt`. The pinned payload SDK
 * declares it, but a declaration is not a capability: this firmware may
 * have removed local-descriptor-table support or filtered `sysarch` down to
 * the fsbase/gsbase operations an ordinary title needs. Measuring that is
 * the purpose of this bounded diagnostic.
 *
 * The pages are reserved low and separately, one writable and one made
 * executable afterwards, so the probe never asks for memory that is
 * writable and executable at once.
 */
#ifndef PROSPERO_WIN_COMPAT32_PS5_H
#define PROSPERO_WIN_COMPAT32_PS5_H

#include "../src/pw_compat32.h"

typedef struct PwCompat32Ps5 {
    uint32_t attempted_bases;   /* how many candidate addresses were tried */
    uint32_t chosen_base;
    int last_errno;
} PwCompat32Ps5;

/* One row of the sysarch argument matrix. */
typedef struct PwLdtAttempt {
    const char *label;
    int op;
    uint32_t start;
    uint32_t num;
    int rc;
    int os_errno;
} PwLdtAttempt;

enum { PW_LDT_ATTEMPTS = 7 };

int pw_compat32_ps5_platform(PwCompat32Ps5 *state,
                             PwCompat32Platform *platform);

/*
 * Runs every argument shape and reports each result, with a known-good
 * sysarch operation as a control, so a uniform failure can be told apart
 * from a malformed call.
 */
int pw_compat32_ps5_diagnose(PwLdtAttempt *out, uint32_t capacity,
                             uint32_t *count);

#endif
