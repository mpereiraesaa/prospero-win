/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PROSPERO_WIN_PREFIX_PS5_H
#define PROSPERO_WIN_PREFIX_PS5_H

#include "../src/pw_prefix.h"

#define PW_PREFIX_PS5_DEFAULT_ROOT "/download0/prospero-win"

/* Adapter for the measured title-storage directory operations. It accepts
 * absolute paths from PwPrefixService and creates each missing component. */
int pw_prefix_ps5_make_directories(void *context,const char *path);
int pw_prefix_ps5_io(PwPrefixIo *io);

#endif
