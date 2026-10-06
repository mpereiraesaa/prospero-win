/* SPDX-License-Identifier: LGPL-2.1-or-later
 * A DIB section over a section the program supplies, as Chromium's software
 * compositor makes one per frame: its bits must be writable by this 32-bit
 * program (patch 0892 keeps them below the WoW64 limit on the PS5). */
#include <windows.h>
#include <stdio.h>

int main(void)
{
    BITMAPINFO info = { { sizeof(BITMAPINFOHEADER), 646, -348, 1, 32, BI_RGB } };
    DWORD size = 646 * 348 * 4;
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, size, NULL);
    HDC dc = GetDC(NULL);
    void *bits = NULL;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, section, 0);
    unsigned int *pixels = bits;

    if (!bitmap || !bits) { printf("no DIB section: %lu\n", GetLastError()); return 1; }
    pixels[0] = 0x00ff00ff;
    pixels[646 * 348 - 1] = 0x00ff00ff;
    printf("DIB section bits at %p, written\n", bits);
    DeleteObject(bitmap);
    CloseHandle(section);
    return 0;
}
