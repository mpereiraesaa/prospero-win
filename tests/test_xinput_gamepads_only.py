#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run patch 0473's caller check: one module sees only gamepads, the rest see all."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "wine/patches/0473-xinput-ps5-gamepads-only-for-one-module.patch"


def added(text):
    return "\n".join(line[1:] for line in text.splitlines()
                     if line.startswith("+") and not line.startswith("+++"))


HARNESS = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
typedef wchar_t WCHAR;
typedef unsigned long DWORD;   /* as in Wine: the patch logs it with %lu */
typedef uint32_t UINT32;
typedef uint16_t UINT16;
typedef long LONG;
typedef uintptr_t ULONG_PTR;
typedef int BOOL, NTSTATUS;
typedef void *HMODULE;
#define TRUE 1
#define FALSE 0
#define WINAPI
#define XUSER_MAX_COUNT 4
#define XINPUT_DEVSUBTYPE_GAMEPAD 1
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MemoryBarrier() __sync_synchronize()
#define InterlockedExchange(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
typedef struct { int done; } INIT_ONCE;
#define INIT_ONCE_STATIC_INIT {0}
typedef BOOL (WINAPI *PINIT_ONCE_FN)(INIT_ONCE *, void *, void **);
static BOOL InitOnceExecuteOnce(INIT_ONCE *once, PINIT_ONCE_FN fn, void *param, void **context)
{
    if (!once->done) { once->done = 1; return fn(once, param, context); }
    return TRUE;
}
static DWORD GetEnvironmentVariableW(const WCHAR *name, WCHAR *out, DWORD size)
{
    const char *value = getenv("WINE_PS5_XINPUT_GAMEPADS_ONLY");
    size_t n;
    assert(!wcscmp(name, L"WINE_PS5_XINPUT_GAMEPADS_ONLY"));
    if (!value) return 0;
    n = strlen(value);
    if (n >= size) return (DWORD)n + 1;   /* Windows: the size needed */
    for (size_t i = 0; i <= n; i++) out[i] = (WCHAR)value[i];
    return (DWORD)n;
}
static int messages;
#define MESSAGE(...) (messages++, printf(__VA_ARGS__))
static const char *debugstr_w(const WCHAR *w) { (void)w; return "the module"; }
/* The engine: an image of 0x1000 bytes, loaded once loaded is set. */
static unsigned char image[0x2000];
static int loaded;
typedef struct { struct { DWORD SizeOfImage; } OptionalHeader; } IMAGE_NT_HEADERS;
static IMAGE_NT_HEADERS headers = { { 0x1000 } };
static HMODULE GetModuleHandleW(const WCHAR *name)
{
    return loaded && !wcscmp(name, L"UnityPlayer.dll") ? (HMODULE)image : NULL;
}
static IMAGE_NT_HEADERS *RtlImageNtHeader(HMODULE module) { return module == image ? &headers : NULL; }
/* The title's controllers: subtype per index, 0 when none. */
struct xinput_pad_caps { UINT32 subtype; UINT16 vendor_id, product_id, revision, flags; };
struct xinput_caps_params { UINT32 index; struct xinput_pad_caps caps; };
enum { unix_pad_caps = 3 };
static UINT32 subtype[4];
#define title_pad 1
static NTSTATUS __wine_unix_call(int handle, int func, void *args)
{
    struct xinput_caps_params *params = args;
    assert(handle == 1 && func == unix_pad_caps && params->index < 4);
    if (!subtype[params->index]) return 3;
    params->caps.subtype = subtype[params->index];
    return 0;
}
""" + "%BLOCK%" + r"""
int main(int argc, char **argv)
{
    const void *engine = image + 0x10, *engine_last = image + 0xfff, *after = image + 0x1000;
    int enabled = argc > 2 && !strcmp(argv[1], "on"), expected = argc > 2 ? atoi(argv[2]) : -1;

    subtype[0] = 1; subtype[1] = 0; subtype[2] = 8; subtype[3] = 7;
    /* Before the engine is loaded nothing is hidden. */
    assert(!title_pad_hidden(3, engine));
    loaded = 1;
    for (int round = 0; round < 3; round++) {
        assert(title_pad_hidden(3, engine) == enabled);       /* guitar */
        assert(title_pad_hidden(3, engine_last) == enabled);
        assert(title_pad_hidden(2, engine) == enabled);       /* drums */
        assert(!title_pad_hidden(0, engine));                 /* gamepad */
        assert(!title_pad_hidden(1, engine));                 /* none there */
        assert(!title_pad_hidden(4, engine));
        assert(!title_pad_hidden(3, after));                  /* HIDrogen and the rest */
        assert(!title_pad_hidden(3, &round));
    }
    assert(messages == expected);                             /* the name, then 3 and 2 once */
    return 0;
}
"""


def main():
    patch = PATCH.read_text()
    main_added = added(patch)
    start = main_added.index("/* WINE_PS5_XINPUT_GAMEPADS_ONLY")
    end = main_added.index("static BOOL title_pad_state(")
    block = main_added[start:end]
    # Every title export passes its own caller: SetState, GetState(Ex),
    # GetKeystroke, the DSound GUIDs, battery, GetCapabilities and
    # GetCapabilitiesEx.
    assert main_added.count("__builtin_return_address(0)") == 8, main_added.count("__builtin_return_address(0)")
    assert "check_for_keystroke(index, keystroke, caller)" in main_added
    assert "if (index >= XUSER_MAX_COUNT || title_pad_hidden(index, caller)) return FALSE;" in main_added
    assert "static DWORD xinput_get_state(DWORD index, XINPUT_STATE *state, const void *caller)" in main_added
    with tempfile.TemporaryDirectory(prefix="pw-xinput-gamepads-") as directory:
        source = Path(directory) / "gamepads.c"
        executable = Path(directory) / "gamepads"
        source.write_text(HARNESS.replace("%BLOCK%", block))
        cc = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run(cc + ["-O2", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                             str(source), "-o", str(executable)], check=True, timeout=60)
        env = dict(os.environ)
        env.pop("WINE_PS5_XINPUT_GAMEPADS_ONLY", None)
        subprocess.run([str(executable), "off", "0"], check=True, timeout=10, env=env, stdout=subprocess.DEVNULL)
        env["WINE_PS5_XINPUT_GAMEPADS_ONLY"] = "UnityPlayer.dll"
        subprocess.run([str(executable), "on", "3"], check=True, timeout=10, env=env, stdout=subprocess.DEVNULL)
        env["WINE_PS5_XINPUT_GAMEPADS_ONLY"] = "Other.dll"
        subprocess.run([str(executable), "off", "1"], check=True, timeout=10, env=env, stdout=subprocess.DEVNULL)
        env["WINE_PS5_XINPUT_GAMEPADS_ONLY"] = "x" * 70 + ".dll"
        subprocess.run([str(executable), "off", "0"], check=True, timeout=10, env=env, stdout=subprocess.DEVNULL)
    print("XInput patch 0473: the named module sees only gamepads; other callers, other names and no name see all")


if __name__ == "__main__":
    main()
