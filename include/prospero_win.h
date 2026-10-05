/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * prospero-win: a zero-emulation Win32 compatibility layer for the PS5.
 *
 * This header is the stable surface shared by the loader core, the host
 * tools and the PS5 adapter. The core deliberately depends on nothing but
 * <stddef.h>, <stdint.h> and <string.h>: no libc locale, no case-folding
 * helper and no allocator is imported from the platform, so a placeholder
 * or subtly wrong Prospero export cannot reach the loader path.
 */
#ifndef PROSPERO_WIN_H
#define PROSPERO_WIN_H

#include <stddef.h>
#include <stdint.h>

#define PROSPERO_WIN_VERSION_MAJOR 0
#define PROSPERO_WIN_VERSION_MINOR 1

/* Every entry point returns PW_OK or a negative pw_result. */
enum pw_result {
    PW_OK = 0,
    PW_ERR_PRECONDITION = -1,   /* caller passed a null or impossible argument */
    PW_ERR_NOT_PE = -2,         /* missing MZ or PE signature */
    PW_ERR_TRUNCATED = -3,      /* a header or table extends past the byte span */
    PW_ERR_MALFORMED = -4,      /* self-inconsistent fields */
    PW_ERR_UNSUPPORTED = -5,    /* well formed, deliberately refused */
    PW_ERR_OVERFLOW = -6,       /* an arithmetic bound would wrap */
    PW_ERR_LIMIT = -7,          /* a compiled capacity was reached */
    PW_ERR_NOT_FOUND = -8,      /* a dependency was not resolvable */
    PW_ERR_VM = -9,             /* the memory backend refused a request */
    PW_ERR_STATE = -10,         /* operation invalid for the current state */
    PW_ERR_X87_TRAP = -11,      /* guest x87 has a pending unmasked exception */
};

/* Page protection requested from a PwVmBackend. */
enum {
    PW_PROT_NONE = 0,
    PW_PROT_READ = 1u << 0,
    PW_PROT_WRITE = 1u << 1,
    PW_PROT_EXEC = 1u << 2,
};

enum {
    PW_MODULE_NAME_MAX = 63,
    PW_PATH_MAX = 255,
};

/* Stable, allocation-free name for a result code; never returns NULL. */
const char *pw_result_name(int result);

/* Name of a PW_PROT_* combination, e.g. "r-x"; never returns NULL. */
const char *pw_protection_name(unsigned protection);

#endif
