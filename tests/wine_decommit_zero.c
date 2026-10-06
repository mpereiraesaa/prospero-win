/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Pages decommitted and committed again read as zeroes, and the committed
 * pages around them keep their contents, also when a host page is larger
 * than a Windows page and holds both (patch 0893; the PS5's are 16 KiB). */
#include <windows.h>
#include <stdio.h>

#define PAGE 4096
#define PAGES 16

static int check(const unsigned char *base, int page, unsigned char value)
{
    int i;

    for (i = 0; i < PAGE; i++)
        if (base[page * PAGE + i] != value)
        {
            printf("decommit page %d byte %d is %#x, expected %#x\n", page, i, base[page * PAGE + i], value);
            return 0;
        }
    return 1;
}

/* Decommit pages [first, first + count) of a committed, filled range and commit them again. */
static int run(unsigned char *base, int first, int count)
{
    MEMORY_BASIC_INFORMATION info;
    int page, ok = 1;

    if (!VirtualAlloc(base, PAGES * PAGE, MEM_COMMIT, PAGE_READWRITE)) return 0;
    for (page = 0; page < PAGES; page++) memset(base + page * PAGE, 0xa0 + page, PAGE);
    if (!VirtualFree(base + first * PAGE, count * PAGE, MEM_DECOMMIT)) return 0;
    if (!VirtualQuery(base + first * PAGE, &info, sizeof(info)) || info.State != MEM_RESERVE ||
        info.RegionSize != (SIZE_T)count * PAGE)
    {
        printf("decommit %d+%d: state %#lx size %#lx\n", first, count, info.State, (unsigned long)info.RegionSize);
        ok = 0;
    }
    if (!VirtualAlloc(base + first * PAGE, count * PAGE, MEM_COMMIT, PAGE_READWRITE)) return 0;
    for (page = 0; page < PAGES; page++)
        ok &= check(base, page, page >= first && page < first + count ? 0 : 0xa0 + page);
    printf("decommit %d+%d %s\n", first, count, ok ? "ok" : "wrong");
    return ok;
}

int main(void)
{
    unsigned char *base = VirtualAlloc(NULL, PAGES * PAGE, MEM_RESERVE, PAGE_NOACCESS);
    int ok = base != NULL;

    ok = ok && run(base, 1, 1);  /* inside one 16 KiB host page */
    ok = ok && run(base, 3, 2);  /* across a 16 KiB host page boundary */
    ok = ok && run(base, 2, 8);  /* partial, whole and partial host pages */
    ok = ok && run(base, 0, 4);  /* exactly one 16 KiB host page */
    printf("decommit verdict=%s\n", ok ? "pass" : "fail");
    fflush(stdout);
    return ok ? 0 : 1;
}
