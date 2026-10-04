#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run patch 0780's get_fqdn against a model of the registry and its change
notifications: it must return what a fresh read returns on every call, and
read the registry again only after a change."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "wine/patches/0780-ws2-32-keep-fqdn-until-the-registry-changes.patch"


def new_side(text, path):
    """The new side of the first hunk that changes path."""
    diff = text.split("diff --git a/" + path + " ", 1)[1].split("\ndiff --git ", 1)[0]
    hunk = diff.split("\n@@", 1)[1].splitlines()[1:]
    return "\n".join(x[1:] for x in hunk if x.startswith((" ", "+")))


source = new_side(PATCH.read_text(), "dlls/ws2_32/protocol.c")
start = source.index("DECLARE_CRITICAL_SECTION(fqdn_cs);")
match = re.search(r"void free_fqdn_cache\(void\)\s*\{", source)
assert match, "free_fqdn_cache"
end, depth = source.index("{", match.start()) + 1, 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
CODE = source[start:end]
socket = new_side(PATCH.read_text(), "dlls/ws2_32/socket.c")
assert "case DLL_PROCESS_DETACH:\n        if (!reserved) free_fqdn_cache();" in socket

HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
typedef wchar_t WCHAR;
typedef uint32_t DWORD;
typedef size_t SIZE_T;
typedef int BOOL, LONG;
typedef void *HKEY, *HANDLE;
typedef enum { ComputerNameNetBIOS, ComputerNameDnsHostname, ComputerNameDnsDomain,
    ComputerNameDnsFullyQualified, ComputerNamePhysicalNetBIOS, ComputerNamePhysicalDnsHostname,
    ComputerNamePhysicalDnsDomain, ComputerNamePhysicalDnsFullyQualified } COMPUTER_NAME_FORMAT;
#define TRUE 1
#define FALSE 0
#define ERROR_MORE_DATA 234
#define ERROR_ACCESS_DENIED 5
#define CP_ACP 0
#define HKEY_LOCAL_MACHINE ((HKEY)(uintptr_t)0x80000002)
#define KEY_NOTIFY 0x10
#define REG_NOTIFY_CHANGE_NAME 1
#define REG_NOTIFY_CHANGE_LAST_SET 4
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define WAIT_FAILED 0xffffffff
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))

/* The critical section: never entered twice, always left. */
static int cs_depth;
#define DECLARE_CRITICAL_SECTION(cs) static int cs
static void EnterCriticalSection(int *cs) { assert(!cs_depth++); (void)cs; }
static void LeaveCriticalSection(int *cs) { assert(cs_depth-- == 1); (void)cs; }

/* The registry: Hostname and Domain below HKLM\System, and the server's
 * notification on our key, which fires once per arming (do_notification). */
static WCHAR hostname[400], domain[100];
static DWORD last_error;
static unsigned reads, opens, arms, waits, events, closes, registrations, step, armed_step, read_step;
static unsigned fail_read, fail_open, fail_arm, fail_event, fail_wait, fail_alloc, fail_conversion;
static unsigned allocations, outstanding, conversions;
static int signalled, key_open, event_open, change_during_read;
static HANDLE the_event = (HANDLE)(uintptr_t)0x40;
static HKEY the_key = (HKEY)(uintptr_t)0x44;
static DWORD GetLastError(void) { return last_error; }
static void SetLastError(DWORD error) { last_error = error; }
static size_t lenW(const WCHAR *s) { size_t n = 0; while (s[n]) n++; return n; }
static int sameW(const WCHAR *a, const WCHAR *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void setW(WCHAR *d, const WCHAR *s) { while ((*d++ = *s++)) {} }

/* Something wrote below HKLM\System. */
static void registry_changed(void)
{
    if (registrations) { signalled = 1; registrations = 0; }
}
static void rename_host(const WCHAR *name) { setW(hostname, name); registry_changed(); }

static HANDLE CreateEventW(void *sa, BOOL manual, BOOL initial, const WCHAR *name)
{
    assert(!sa && !manual && !initial && !name && !event_open);
    if (fail_event) return NULL;
    events++; event_open = 1; signalled = 0;
    return the_event;
}
static LONG RegOpenKeyExW(HKEY root, const WCHAR *name, DWORD options, DWORD access, HKEY *key)
{
    assert(root == HKEY_LOCAL_MACHINE && sameW(name, L"System") && !options && access == KEY_NOTIFY);
    assert(!key_open);
    opens++;
    if (fail_open) { *key = (HKEY)(uintptr_t)0xbad; return ERROR_ACCESS_DENIED; }
    key_open = 1; *key = the_key;
    return 0;
}
static LONG RegNotifyChangeKeyValue(HKEY key, BOOL subtree, DWORD filter, HANDLE event, BOOL async)
{
    assert(key == the_key && key_open && event == the_event && event_open && async && subtree);
    assert(filter == (REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET));
    arms++;
    if (fail_arm) return ERROR_ACCESS_DENIED;
    registrations++; signalled = 0;  /* the server resets the event it adds */
    armed_step = ++step;
    return 0;
}
static DWORD WaitForSingleObject(HANDLE event, DWORD timeout)
{
    assert(event == the_event && event_open && !timeout);
    waits++;
    if (fail_wait) return WAIT_FAILED;
    if (!signalled) return WAIT_TIMEOUT;
    signalled = 0;  /* auto-reset */
    return WAIT_OBJECT_0;
}
static BOOL CloseHandle(HANDLE handle)
{
    closes++;
    if (handle == the_key) { assert(key_open); key_open = 0; registrations = 0; }
    else { assert(handle == the_event && event_open); event_open = 0; }
    return TRUE;
}
static BOOL GetComputerNameExW(COMPUTER_NAME_FORMAT type, WCHAR *name, DWORD *len)
{
    size_t h = lenW(hostname), d = lenW(domain), n = h + (d ? d + 1 : 0);

    assert(type == ComputerNamePhysicalDnsFullyQualified);
    reads++; read_step = ++step;
    if (fail_read) { SetLastError(5); return FALSE; }
    if (!name || *len <= n) { *len = (DWORD)(n + 1); SetLastError(ERROR_MORE_DATA); return FALSE; }
    memcpy(name, hostname, h * sizeof(WCHAR));
    if (d) { name[h] = '.'; memcpy(name + h + 1, domain, d * sizeof(WCHAR)); }
    name[n] = 0; *len = (DWORD)n;
    if (change_during_read) { change_during_read = 0; rename_host(L"late"); }
    return TRUE;
}
static int WideCharToMultiByte(unsigned cp, DWORD flags, const WCHAR *s, int len, char *out, int capacity,
                               const char *def, BOOL *used)
{
    char bytes[2000]; size_t n = 0;
    assert(cp == CP_ACP && !flags && len == -1 && !def && !used);
    if (++conversions == fail_conversion) return 0;
    while (*s) { unsigned c = *s++; assert(c < 0x80); bytes[n++] = (char)c; }
    bytes[n++] = 0;
    if (out) { if (capacity < (int)n) return 0; memcpy(out, bytes, n); }
    return (int)n;
}
static void *tracked_malloc(size_t bytes)
{ void *p; if (++allocations == fail_alloc) return NULL; if ((p = malloc(bytes))) outstanding++; return p; }
static void tracked_free(void *p) { if (p) { assert(outstanding); outstanding--; free(p); } }
#define malloc tracked_malloc
#define free tracked_free
/*CODE*/
#undef malloc
#undef free

/* The name a fresh read gives now, without touching the counters. */
static void fresh(char *out)
{
    size_t i = 0, h = lenW(hostname), d = lenW(domain);
    for (size_t j = 0; j < h; j++) out[i++] = (char)hostname[j];
    if (d) { out[i++] = '.'; for (size_t j = 0; j < d; j++) out[i++] = (char)domain[j]; }
    out[i] = 0;
}
static void expect_fresh(void)
{
    char want[1200], *got;
    fresh(want);
    got = get_fqdn();
    assert(got && !strcmp(got, want));
    tracked_free(got);
    assert(!cs_depth);
}
static void reset(void)
{
    free_fqdn_cache();
    assert(!outstanding && !key_open && !event_open && !registrations && !cs_depth);
    reads = opens = arms = waits = events = closes = 0;
    fail_read = fail_open = fail_arm = fail_event = fail_wait = fail_alloc = fail_conversion = 0;
    allocations = conversions = 0;
    setW(hostname, L"alpha"); setW(domain, L"example");
}
static unsigned seed = 12345;
static unsigned next_random(void) { seed = seed * 1103515245u + 12345u; return seed >> 16; }

int main(void)
{
    _Static_assert(sizeof(WCHAR) == 2, "Wine uses UTF-16");

    /* One read, then none while nothing changes; one wait per call. */
    reset();
    expect_fresh();
    assert(reads == 1 && opens == 1 && arms == 1 && events == 1 && armed_step < read_step);
    for (int i = 0; i < 1000; i++) expect_fresh();
    assert(reads == 1 && arms == 1 && opens == 1 && waits == 1000);

    /* A rename is seen by the next call: one read, one new arm before it. */
    rename_host(L"beta");
    expect_fresh();
    assert(reads == 2 && arms == 2 && opens == 1 && armed_step < read_step);
    expect_fresh();
    assert(reads == 2);

    /* A write elsewhere below HKLM\System costs one extra read. */
    registry_changed();
    expect_fresh();
    assert(reads == 3 && arms == 3);

    /* A change right after the read (the watch was already armed). */
    change_during_read = 1;
    registry_changed();
    { char *s = get_fqdn(); assert(s && !strcmp(s, "beta.example")); tracked_free(s); }
    expect_fresh();
    assert(reads == 5);

    /* No domain; long names that need the second, larger read. */
    reset(); domain[0] = 0; expect_fresh(); expect_fresh(); assert(reads == 1);
    reset(); for (int i = 0; i < 300; i++) hostname[i] = 'x';
    hostname[300] = 0; expect_fresh(); assert(reads == 2); expect_fresh(); assert(reads == 2);

    /* Without a watch every call reads, as before the patch. */
    reset(); fail_open = 1;
    for (int i = 0; i < 5; i++) expect_fresh();
    assert(reads == 5 && opens == 5 && !arms && !key_open);
    reset(); fail_arm = 1;
    for (int i = 0; i < 5; i++) expect_fresh();
    assert(reads == 5 && opens == 1 && arms == 5 && !registrations);
    reset(); fail_event = 1;
    for (int i = 0; i < 5; i++) expect_fresh();
    assert(reads == 5 && !opens && !arms);

    /* A failed wait counts as a change. */
    reset(); expect_fresh(); fail_wait = 1; expect_fresh(); expect_fresh();
    assert(reads == 3 && arms == 3);

    /* A failed read is not kept, and does not arm the watch again. */
    reset(); fail_read = 1;
    assert(!get_fqdn() && !get_fqdn());
    assert(reads == 2 && arms == 1 && registrations == 1);
    fail_read = 0; expect_fresh(); expect_fresh();
    assert(reads == 3 && arms == 1);

    /* Allocation and conversion failures: NULL, nothing leaked, the kept
     * name still right afterwards. */
    reset(); fail_alloc = 1; assert(!get_fqdn() && !outstanding); expect_fresh();
    reset(); expect_fresh(); fail_alloc = allocations + 1; assert(!get_fqdn()); expect_fresh();
    reset(); expect_fresh(); fail_conversion = conversions + 1; assert(!get_fqdn()); expect_fresh();
    reset(); expect_fresh(); fail_conversion = conversions + 2; assert(!get_fqdn()); expect_fresh();
    assert(reads == 1);

    /* Random interleavings: always what a fresh read returns, and a read
     * only after a change. */
    reset();
    {
        static const WCHAR *names[] = { L"alpha", L"beta", L"gamma" };
        unsigned changes = 1;
        for (int i = 0; i < 20000; i++)
        {
            switch (next_random() % 8)
            {
            case 0: rename_host(names[next_random() % 3]); changes++; break;
            case 1: registry_changed(); changes++; break;
            case 2: setW(domain, next_random() % 2 ? L"example" : L""); registry_changed(); changes++; break;
            default: expect_fresh(); break;
            }
        }
        assert(reads <= changes);
    }

    /* Unloading closes the key and the event and frees the name. */
    expect_fresh();
    free_fqdn_cache();
    assert(!key_open && !event_open && !outstanding);
    free_fqdn_cache();
    expect_fresh();
    reset();
    puts("fqdn cache passed: fresh on every call, re-read only after a registry change, no leaks");
    return 0;
}
'''


def run():
    code = HARNESS.replace("/*CODE*/", CODE)
    with tempfile.TemporaryDirectory() as directory:
        source_path, exe = Path(directory) / "fqdn_cache.c", Path(directory) / "fqdn_cache"
        source_path.write_text(code)
        command = shlex.split(os.environ.get("CC", "cc")) + shlex.split(os.environ.get("CFLAGS", "-O2"))
        command += ["-std=gnu11", "-fshort-wchar", "-Wno-unused-function", "-Wno-unused-parameter"]
        subprocess.run(command + [str(source_path), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


run()
