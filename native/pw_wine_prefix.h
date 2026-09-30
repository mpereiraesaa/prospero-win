/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_WINE_PREFIX_H
#define PW_WINE_PREFIX_H
#include "../src/pw_prefix_temp.h"

/* Reads <prefix>/user.reg, if there is one, and creates the temp folders it
 * names and C:\windows\temp under <prefix>/drive_c (src/pw_prefix_temp.h).
 * Returns how many folders there are afterwards, or -1 when one could not be
 * created, as for a prefix Wine has not made yet. */
int pw_wine_prefix_temp_create(const char *prefix, PwPrefixTemp *temp);

#endif
