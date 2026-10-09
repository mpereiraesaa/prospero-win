/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_STAGING_H
#define PW_D3D9_STAGING_H
#include <stdint.h>
/* Shared PE32 buffer/texture staging budget, process-wide and atomic.
 * Allocation returns an owned complete span below4GiB or a failure HRESULT.
 * Free accepts only the original pointer/size pair from a successful alloc. */
uint32_t pw_d3d9_staging_alloc(uint32_t,void **);
uint32_t pw_d3d9_staging_free(void *,uint32_t);
uint32_t pw_d3d9_staging_bytes(void);
#endif
