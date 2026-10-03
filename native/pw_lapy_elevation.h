/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_LAPY_ELEVATION_H
#define PW_LAPY_ELEVATION_H

#include <stdint.h>

/* Send the bundled one-shot ELF to the local elfldr listener and wait for the
 * versioned result for this process. Returns 0 on success, -1 with errno. */
int pw_lapy_elevation_request(int32_t pid);

#endif
