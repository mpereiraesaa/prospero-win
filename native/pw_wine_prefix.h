/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_WINE_PREFIX_H
#define PW_WINE_PREFIX_H
#include "../src/pw_prefix_temp.h"

/* Reads <prefix>/user.reg, if there is one, and creates the temp folders it
 * names and C:\windows\temp under <prefix>/drive_c (src/pw_prefix_temp.h).
 * Returns how many folders there are afterwards, or -1 when one could not be
 * created, as for a prefix Wine has not made yet. */
int pw_wine_prefix_temp_create(const char *prefix, PwPrefixTemp *temp);

/* WoW64 loads its CPU backend from the prefix's system32 only, so a prefix
 * made before the native backend shipped lacks it. Copies source (the
 * runtime's x86_64-windows/<name>) to <prefix>/drive_c/windows/system32/<name>
 * when that file is missing or differs, through a temporary file renamed into
 * place. Returns 1 when it copied, 0 when the prefix already had the same
 * file, -1 on any failure (Wine then falls back to the prefix's own CPU). */
int pw_wine_prefix_cpu_install(const char *prefix, const char *source, const char *name);

#endif
