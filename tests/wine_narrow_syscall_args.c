/* SPDX-License-Identifier: LGPL-2.1-or-later
 * A BOOLEAN or WCHAR system call argument whose register has stale bits
 * above it means what its low bits say: the Windows x64 ABI leaves those
 * bits undefined, and a caller need not clear them. ntdll's wrappers cover
 * the BOOLEANs, patch 0894 win32u's WCHAR. */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>

typedef NTSTATUS (WINAPI *delay_fn)(ULONG_PTR alertable, LARGE_INTEGER *timeout);
typedef NTSTATUS (WINAPI *wait_fn)(HANDLE handle, ULONG_PTR alertable, LARGE_INTEGER *timeout);
typedef WORD (WINAPI *vkscan_fn)(ULONG_PTR chr, HKL layout);

#define STALE_FALSE 0x100 /* FALSE in its byte, a bit set above it */

static volatile LONG apcs;

static void CALLBACK apc(ULONG_PTR arg)
{
    (void)arg;
    apcs++;
}

int main(void)
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    HMODULE win32u = LoadLibraryA("win32u.dll");
    delay_fn delay = (delay_fn)GetProcAddress(ntdll, "NtDelayExecution");
    wait_fn wait = (wait_fn)GetProcAddress(ntdll, "NtWaitForSingleObject");
    vkscan_fn vkscan = win32u ? (vkscan_fn)GetProcAddress(win32u, "NtUserVkKeyScanEx") : NULL;
    LARGE_INTEGER timeout = { .QuadPart = -10000 }; /* 1 ms */
    HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
    HKL layout = GetKeyboardLayout(0);
    NTSTATUS delay_status, wait_status;
    LONG delay_apcs, wait_apcs;
    WORD scan = 0, expected = VkKeyScanExW(L'A', layout);
    int ok;

    QueueUserAPC(apc, GetCurrentThread(), 0);
    delay_status = delay(STALE_FALSE, &timeout); /* not alertable: the APC waits */
    delay_apcs = apcs;
    SleepEx(0, TRUE);
    QueueUserAPC(apc, GetCurrentThread(), 0);
    wait_status = wait(event, STALE_FALSE, &timeout);
    wait_apcs = apcs - 1;
    SleepEx(0, TRUE);
    if (vkscan) scan = vkscan(0x10000 | L'A', layout);

    ok = delay_status == 0 /* STATUS_SUCCESS */ && !delay_apcs && wait_status == STATUS_TIMEOUT && !wait_apcs &&
         apcs == 2 && vkscan && scan == expected;
    printf("narrow-args delay=%#lx apcs=%ld wait=%#lx apcs=%ld vkscan=%#x expected=%#x\n",
           delay_status, delay_apcs, wait_status, wait_apcs, scan, expected);
    printf("narrow-args verdict=%s\n", ok ? "pass" : "fail");
    fflush(stdout);
    return ok ? 0 : 1;
}
