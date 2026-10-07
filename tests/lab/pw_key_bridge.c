/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <windows.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pw_key_shared.h"

struct ntuser_thread_info
{
    uint8_t original[32];
    UINT64 ps5_key_shared[6], ps5_key_disabled;
};
struct input { uint64_t seq, id; uint8_t state[256]; uint32_t locked, pad; uint64_t serial; };
struct desktop { uint64_t seq, id, serial; };
static struct input *input;
static struct desktop *desktop;
static DWORD slot;
static LONG slows, queries, asyncs;
static int supported = 1, set_result = 1;

static struct ntuser_thread_info *mock_info(void)
{
    struct ntuser_thread_info *info = TlsGetValue(slot);
    if (!info)
    {
        info = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*info));
        assert(info && TlsSetValue(slot, info));
    }
    return info;
}

static SHORT WINAPI pw_slow_NtUserGetKeyState(INT key)
{
    (void)key;
    InterlockedIncrement(&slows);
    return 0x1234;
}

static SHORT WINAPI pw_slow_NtUserGetAsyncKeyState(INT key)
{
    (void)key;
    InterlockedIncrement(&asyncs);
    return (SHORT)0x8001;
}

static BOOL WINAPI pw_slow_NtUserSetThreadDesktop(HDESK handle)
{
    (void)handle;
    return set_result;
}

static ULONG_PTR mock_query(ULONG_PTR pointer, ULONG_PTR size, ULONG code)
{
    struct pw_key_shared *d = (void *)pointer;
    assert(code == PW_KEY_QUERY && size == sizeof(*d));
    InterlockedIncrement(&queries);
    if (!supported) return 0;
    memset(d, 0, sizeof(*d));
    d->version = PW_KEY_VERSION;
    d->input_object = (uintptr_t)input; d->desktop_object = (uintptr_t)desktop;
    d->input_id = input->id; d->desktop_id = desktop->id;
    d->state = (uintptr_t)input->state; d->lock = (uintptr_t)&input->locked;
    d->input_serial = (uintptr_t)&input->serial; d->desktop_serial = (uintptr_t)&desktop->serial;
    return 1;
}

#define NtUserGetThreadInfo mock_info
#define NtUserCallTwoParam mock_query
#include "key_body.inc"

static DWORD WINAPI worker(void *unused)
{
    unsigned i;
    struct ntuser_thread_info *info;
    (void)unused;
    assert(NtUserGetKeyState(65) == 0x1234);
    for (i = 0; i < 10000; ++i) assert(NtUserGetKeyState(65) == -128);
    info = mock_info();
    TlsSetValue(slot, NULL);
    HeapFree(GetProcessHeap(), 0, info);
    return 0;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    slot = TlsAlloc();
    assert(slot != TLS_OUT_OF_INDEXES);
    input = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(input && (uintptr_t)input <= UINT32_MAX - 4096);
    desktop = (void *)((char *)input + 1024);
    input->id = 31; desktop->id = 41;
    input->serial = desktop->serial = 71;
    input->state[65] = 0x80;
    if (!strcmp(argv[1], "unsupported"))
    {
        supported = 0;
        assert(NtUserGetKeyState(65) == 0x1234);
        assert(NtUserGetKeyState(65) == 0x1234);
        assert(slows == 2 && queries == 1);
    }
    else if (!strcmp(argv[1], "concurrent"))
    {
        HANDLE threads[8];
        unsigned i;
        for (i = 0; i < 8; ++i) threads[i] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        assert(WaitForMultipleObjects(8, threads, TRUE, 20000) == WAIT_OBJECT_0);
        for (i = 0; i < 8; ++i) CloseHandle(threads[i]);
        assert(slows == 8 && queries == 8);
    }
    else
    {
        assert(NtUserGetKeyState(65) == 0x1234);
        assert(NtUserGetKeyState(65) == -128 && slows == 1 && queries == 1);
        input->state[65] = 1;
        assert(NtUserGetKeyState(65 + 256) == 1);
        ++desktop->serial;
        assert(NtUserGetKeyState(65) == 0x1234 && slows == 2);
        input->locked = 1;
        assert(NtUserGetKeyState(65) == 1);
        ++input->id;
        assert(NtUserGetKeyState(65) == 0x1234 && slows == 3);
        assert(NtUserGetKeyState(65) == 1);
        set_result = 0;
        assert(!NtUserSetThreadDesktop(NULL));
        assert(NtUserGetKeyState(65) == 1 && slows == 3);
        set_result = 1;
        assert(NtUserSetThreadDesktop(NULL));
        assert(NtUserGetKeyState(65) == 0x1234 && slows == 4);
        assert(NtUserGetAsyncKeyState(65) == (SHORT)0x8001 && asyncs == 1);
        assert(NtUserGetAsyncKeyState(-1) == 0 && NtUserGetAsyncKeyState(256) == 0 && asyncs == 1);
    }
    HeapFree(GetProcessHeap(), 0, TlsGetValue(slot));
    TlsFree(slot);
    VirtualFree(input, 0, MEM_RELEASE);
    puts("PASS actual PE shared-key wrappers: fallback, synchronization, desktop and async semantics");
    return 0;
}
