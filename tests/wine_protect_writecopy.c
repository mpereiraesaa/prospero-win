/* SPDX-License-Identifier: LGPL-2.1-or-later
 * A page of this image's data, written, then protected read-only: Windows
 * reports its old protection as PAGE_READWRITE (4), not PAGE_WRITECOPY (8).
 * Chromium's embedded browser checks exactly that (patch 0891). */
#include <windows.h>
#include <stdio.h>

static volatile unsigned char page[8192] __attribute__((aligned(4096)));

int main(void)
{
    DWORD old = 0;
    BOOL ok;

    page[0] = 1;
    ok = VirtualProtect((void *)page, 4096, PAGE_READONLY, &old);
    printf("VirtualProtect ok=%d old=%#lx\n", ok, old);
    return ok && old == PAGE_READWRITE ? 0 : 1;
}
