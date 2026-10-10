/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef NATIVE_LIBKERNEL_RESOLVER_H
#define NATIVE_LIBKERNEL_RESOLVER_H
#include <stddef.h>
#include <stdint.h>
struct native_libkernel_entries {
    uintptr_t sigaction, sigreturn, sysarch;
};
/* Pure metadata guard: never reads code or calls a candidate. Unknown
 * identities fail closed and leave all output entries zero. */
int native_libkernel_resolve(const void *info, size_t length,
                            uintptr_t sigaction_import, uintptr_t sysarch_import,
                            struct native_libkernel_entries *entries);
#endif
