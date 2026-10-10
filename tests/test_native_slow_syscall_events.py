#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""The slow-system-service hook in wine/patches/0613, compiled on the host
against stubs: off unless PW_NATIVE_SLOW_SYSCALL_US is a positive decimal,
the threshold from PW_QPC_TSC_HZ (1.6 GHz without it), the dispatch and its
result unchanged, one line per slow call with the thunk continuation, its
caller and the module-resolved dwords of a synthetic 32-bit stack, the
bounds of that scan, the name mapping and the line's size cap."""
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
PATCH = ROOT / "wine/patches/0613-wow64-slow-system-service-events.patch"
HOOK = "    if (pw_slow_syscall_state == PW_SLOW_SYSCALL_OFF)"

PRELUDE = r"""
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>
#include <stdarg.h>
#include <assert.h>
#include <time.h>
#include <sys/mman.h>
typedef uint64_t ULONGLONG;
typedef uintptr_t ULONG_PTR;
typedef unsigned int ULONG, UINT, BOOL;
typedef int LONG, NTSTATUS;
typedef wchar_t WCHAR;
typedef struct {unsigned short Length, MaximumLength; WCHAR *Buffer;} UNICODE_STRING;
typedef struct {unsigned short Length, MaximumLength; ULONG Buffer;} UNICODE_STRING32;
typedef struct {ULONG Flink, Blink;} LIST_ENTRY32;
typedef struct {ULONG Length; ULONG SsHandle; LIST_ENTRY32 InLoadOrderModuleList, InMemoryOrderModuleList;} PEB_LDR_DATA32;
typedef struct {ULONG Reserved[3]; ULONG LdrData;} PEB32;
typedef struct {ULONG ExceptionList, StackBase, StackLimit;} NT_TIB32;
typedef struct {NT_TIB32 Tib;} TEB32;
typedef struct {int done;} RTL_RUN_ONCE;
#define RTL_RUN_ONCE_INIT {0}
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define FIELD_OFFSET(type, field) offsetof(type, field)
#define ULongToPtr(x) ((void *)(uintptr_t)(x))
#define WINAPI
#define TRUE 1
#define FALSE 0
#define ProcessWow64Information 26
#define GetCurrentProcess() ((void *)-1)
struct teb {struct {void *UniqueThread;} ClientId;};
static struct teb current_teb = {{(void *)0x4c}};
static TEB32 current_teb32;
static ULONG_PTR highest_user_address = 0x7ffeffff;
static const char *const pw_syscall_names[] = {"NtYieldExecution", "NtWaitForSingleObject"};
static const WCHAR *env_us, *env_hz;
static PEB32 *fake_peb32;
static int query_fails, messages, slow_ns;
static char last_message[2048];
static UINT *called_args; static ULONG_PTR called_thunk;
static struct teb *NtCurrentTeb(void){return &current_teb;}
static TEB32 *NtCurrentTeb32(void){return &current_teb32;}
static void RtlInitUnicodeString(UNICODE_STRING *s,const WCHAR *p){s->Buffer=(WCHAR *)p;s->Length=wcslen(p)*sizeof(WCHAR);}
static int RtlQueryEnvironmentVariable_U(void *e,UNICODE_STRING *n,UNICODE_STRING *v)
{
    const WCHAR *text = !wcscmp(n->Buffer, L"PW_NATIVE_SLOW_SYSCALL_US") ? env_us : !wcscmp(n->Buffer, L"PW_QPC_TSC_HZ") ? env_hz : NULL;
    (void)e;
    if (!text) return 1;
    assert(wcslen(text) * sizeof(WCHAR) <= v->MaximumLength);
    wcscpy(v->Buffer, text); v->Length = wcslen(text) * sizeof(WCHAR);
    return 0;
}
static int RtlRunOnceExecuteOnce(RTL_RUN_ONCE *o,ULONG (*fn)(RTL_RUN_ONCE *,void *,void **),void *a,void **c){if(!o->done){o->done=1;fn(o,a,c);}return 0;}
static LONG InterlockedExchange(LONG *p, LONG v){LONG old=*p;*p=v;return old;}
static NTSTATUS NtQueryInformationProcess(void *process, int class, void *out, ULONG size, ULONG *ret)
{
    (void)process; (void)ret; assert(class == ProcessWow64Information && size == sizeof(void *));
    if (query_fails) return -1;
    *(PEB32 **)out = fake_peb32; return 0;
}
static NTSTATUS wow64_syscall(UINT *args, ULONG_PTR thunk)
{
    struct timespec ts = {0, slow_ns};
    called_args = args; called_thunk = thunk;
    if (slow_ns) nanosleep(&ts, NULL);
    return (NTSTATUS)0xc0000005;
}
static void message(const char *fmt,...)
{
    char format[512]; const char *in = fmt; char *out = format; va_list a;
    /* ULONG is 32 bits here: drop the l length modifiers the real build needs. */
    while (*in) { if (in[0] == 'l' && in[1] == 'x') { in++; continue; } *out++ = *in++; }
    *out = 0;
    va_start(a, fmt); vsnprintf(last_message, sizeof(last_message), format, a); va_end(a);
    messages++;
}
#define MESSAGE message
"""

MAIN = r"""
#define REGION 0x10000000u
#define TEXT(n) (REGION + 0x1000 + (n) * 0x100)
struct image { ULONG base, size; const wchar_t *name; };
static const struct image images[] = {
    { 0x7bc00000, 0x00100000, L"ntdll.dll" }, { 0x00400000, 0x00800000, L"GTAIV.exe" }, { 0, 0x1000, L"zero.dll" },
    { 0x10000000, 0x00300000, L"D3D9.DLL" }, { 0x64000000, 0x00010000, L"nameless" },
    { 0x70000000, 0x00001000, L"a-very-long-module-name-that-fills-the-line-up-to-its-cap-xx.dll" } };
static ULONG *stack; static ULONG esp;
static void build_region(int long_names)
{
    void *region = mmap((void *)(uintptr_t)REGION, 0x100000, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    PEB_LDR_DATA32 *ldr = (PEB_LDR_DATA32 *)(uintptr_t)(REGION + 0x100);
    struct pw_slow_syscall_ldr_entry32 *entries = (struct pw_slow_syscall_ldr_entry32 *)(uintptr_t)(REGION + 0x200);
    ULONG head = REGION + 0x100 + offsetof(PEB_LDR_DATA32, InLoadOrderModuleList), prev = head;
    unsigned i;
    assert(region == (void *)(uintptr_t)REGION);
    fake_peb32 = (PEB32 *)(uintptr_t)REGION;
    fake_peb32->LdrData = REGION + 0x100;
    for (i = 0; i < ARRAY_SIZE(images); ++i)
    {
        wchar_t *name = (wchar_t *)(uintptr_t)TEXT(i);
        ULONG link = REGION + 0x200 + i * sizeof(*entries);
        wcscpy(name, images[i].name);
        entries[i].DllBase = images[i].base; entries[i].SizeOfImage = images[i].size;
        entries[i].BaseDllName.Buffer = i == 4 ? 0 : TEXT(i);
        entries[i].BaseDllName.Length = i == 4 ? 0 : wcslen(name) * sizeof(WCHAR);
        *(ULONG *)(uintptr_t)(prev) = link;      /* previous Flink */
        entries[i].InLoadOrderLinks.Blink = prev;
        prev = link;
    }
    *(ULONG *)(uintptr_t)prev = head;
    ldr->InLoadOrderModuleList.Blink = prev;
    /* The guest stack: 0x2000 dwords, the service entered with ESP 0x400 dwords above its limit. */
    stack = (ULONG *)(uintptr_t)(REGION + 0x10000);
    current_teb32.Tib.StackLimit = REGION + 0x10000;
    current_teb32.Tib.StackBase = REGION + 0x10000 + 0x2000 * 4;
    esp = REGION + 0x10000 + 0x400 * 4;
    memset(stack, 0, 0x2000 * 4);
    stack[0x400] = 0x7bc01234;                 /* thunk continuation: ntdll.dll+1234 */
    stack[0x401] = long_names ? 0x70000010 : 0x101a2b3c;   /* the caller */
    stack[0x402] = 0x00000001; stack[0x403] = 0x7bc0ffff; stack[0x404] = 0x00402000;   /* arg0..2 */
    for (i = 0; i < 20; ++i) stack[0x410 + 3 * i] = long_names ? 0x70000020 + i : 0x10000100 + i;   /* 20 candidates */
    stack[0x405] = 0x64000020;                 /* the module without a name */
    stack[0x406] = 0x00000800;                 /* below every module */
    stack[0x407] = 0x7bd00000;                 /* just past ntdll */
    stack[0x400 + 600] = 0x7bc00009;           /* beyond the 512-dword scan */
    stack[0x1fff] = 0x7bc0000a;                /* the last dword before StackBase: in range, but after the 14 */
}
static NTSTATUS call(int slow)
{
    NTSTATUS status;
    slow_ns = slow ? 250000000 : 0;
    messages = 0;
    status = Wow64SystemServiceEx(1, stack + 0x402);
    assert(called_args == stack + 0x402 && called_thunk == 0xabc && status == (NTSTATUS)0xc0000005);
    return status;
}
int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";
    build_region(!strcmp(mode, "long"));
    if (!strcmp(mode, "off"))
    {
        call(1);
        assert(pw_slow_syscall_state == PW_SLOW_SYSCALL_OFF && !pw_slow_syscall_threshold && !messages);
        assert(!pw_slow_syscall_peb32);
        return 0;
    }
    if (!strcmp(mode, "zero")) env_us = L"0";
    else if (!strcmp(mode, "words")) env_us = L"12ms";
    else if (!strcmp(mode, "empty")) env_us = L"";
    else if (!strcmp(mode, "toolong")) env_us = L"12345678901234567890";
    if (env_us) { call(1); assert(pw_slow_syscall_state == PW_SLOW_SYSCALL_OFF && !pw_slow_syscall_threshold && !messages); return 0; }
    if (!strcmp(mode, "hz"))
    {
        env_us = L"250"; env_hz = L"3000000000";
        call(0);
        assert(pw_slow_syscall_state == PW_SLOW_SYSCALL_ON && pw_slow_syscall_threshold == 250ull * 3000 && !messages);
        return 0;
    }
    if (!strcmp(mode, "badhz"))
    {
        env_us = L"250"; env_hz = L"12";
        call(0);
        assert(pw_slow_syscall_threshold == 250ull * 1600 && !messages);
        return 0;
    }
    if (!strcmp(mode, "nopeb"))
    {
        env_us = L"100000"; query_fails = 1;
        call(1);
        assert(messages == 1 && strstr(last_message, " stack=7bc01234,101a2b3c\n"));
        return 0;
    }
    env_us = L"100000";     /* 100 ms at the 1.6 GHz fallback */
    call(0);
    assert(pw_slow_syscall_state == PW_SLOW_SYSCALL_ON && pw_slow_syscall_threshold == 160000000ull && !messages);
    assert(pw_slow_syscall_peb32 == fake_peb32);
    call(1);
    assert(messages == 1);
    printf("%s", last_message);
    return 0;
}
"""


def added_lines() -> str:
    text = PATCH.read_text()
    assert HOOK in text, "the hook line moved"
    added = "\n".join(line[1:] for line in text.splitlines() if line.startswith("+") and not line.startswith("+++"))
    assert HOOK in added
    return added[:added.index(HOOK)]


# The dispatcher as the hook leaves it, with the wow64_syscall call of the original.
DISPATCH = r"""
NTSTATUS Wow64SystemServiceEx( UINT num, UINT *args )
{
    NTSTATUS status;
    if (pw_slow_syscall_state == PW_SLOW_SYSCALL_OFF) status = wow64_syscall( args, 0xabc );
    else status = pw_slow_syscall_dispatch( num, args, 0xabc );
    return status;
}
"""


class NativeSlowSyscallEvents(unittest.TestCase):
    def test_patch_shape(self):
        text = PATCH.read_text()
        self.assertTrue(text.startswith("From: prospero-win contributors\nSubject: [PATCH] wow64: "))
        self.assertIn("--- a/dlls/wow64/syscall.c\n+++ b/dlls/wow64/syscall.c\n", text)
        self.assertIn("-    status = wow64_syscall( args, table->ServiceTable[id] );\n", text)
        self.assertIn("+    else status = pw_slow_syscall_dispatch( num, args, table->ServiceTable[id] );\n", text)
        self.assertIn('MESSAGE("PW_NATIVE_SLOW_SYSCALL version=1 tid=%04lx tsc=%llu ticks=%llu code=%04x name=%s '
                      'arg0=%08lx arg1=%08lx arg2=%08lx status=%08lx stack=%s\\n"', text)
        self.assertIn("ALL_SYSCALLS32", (ROOT / "wine/patches/0612-wow64-opt-in-named-system-service-profile.patch").read_text())

    def test_hook_on_the_host(self):
        with tempfile.TemporaryDirectory() as td:
            source = pathlib.Path(td) / "events.c"
            binary = pathlib.Path(td) / "events"
            source.write_text(PRELUDE + added_lines() + DISPATCH + MAIN)
            subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                            "-fno-sanitize-recover=all", str(source), "-o", str(binary)], check=True)
            for mode in ("off", "zero", "words", "empty", "toolong", "hz", "badhz", "nopeb"):
                subprocess.run([str(binary), mode], check=True, capture_output=True)
            run = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            line = run.stdout
            self.assertTrue(line.startswith("PW_NATIVE_SLOW_SYSCALL version=1 tid=004c tsc="), line)
            self.assertRegex(line, r" tsc=\d+ ticks=\d+ code=0001 name=NtWaitForSingleObject arg0=00000001 "
                                   r"arg1=7bc0ffff arg2=00402000 status=c0000005 stack=")
            stack = line.split("stack=", 1)[1].rstrip("\n").split(",")
            # The thunk continuation and its caller first, then the arguments that
            # fall in a module, the nameless module, then the candidates in stack
            # order, stopping at 14 found: the far and the late ones are left out.
            self.assertEqual(stack[:5], ["ntdll.dll+1234", "d3d9.dll+1a2b3c", "ntdll.dll+ffff", "gtaiv.exe+2000",
                                         "module+20"])
            self.assertEqual(stack[5:], [f"d3d9.dll+{0x100 + i:x}" for i in range(11)])
            self.assertEqual(len(stack), 2 + 14)
            self.assertNotIn("zero.dll", line)
            ticks = int(line.split("ticks=", 1)[1].split()[0])
            self.assertGreater(ticks, 160000000)
            # A long module name fills the 800-character cap: the stack ends in ... and the line stays under 1000 bytes.
            run = subprocess.run([str(binary), "long"], check=True, capture_output=True, text=True)
            self.assertTrue(run.stdout.rstrip("\n").endswith("..."), run.stdout)
            self.assertLess(len(run.stdout.encode()), 1000)
            self.assertIn("a-very-long-module-name-that-fills-the-line-up-to-its-cap-xx.dll+10,", run.stdout)


if __name__ == "__main__":
    unittest.main()
