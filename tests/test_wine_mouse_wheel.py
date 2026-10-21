#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Execute the actual PS5 driver's wheel helper with a mocked input sink."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "wine/patches/0671-win32u-ps5-mouse-wheel.patch"


def main():
    # The helper is wholly added by the patch. Compile that body rather than
    # a model, so changing its scale, sign, flags or bounds breaks this test.
    added = "\n".join(line[1:] for line in PATCH.read_text().splitlines()
                      if line.startswith("+") and not line.startswith("+++"))
    start = added.index("static void send_wheel(")
    end = added.index("\n}", start) + 2
    helper = added[start:end]
    harness = r'''
#include <stdint.h>
#include <assert.h>
#include <limits.h>
#include <string.h>
typedef int INT;
typedef uint32_t DWORD;
typedef uintptr_t HWND;
#define INPUT_MOUSE 0
#define MOUSEEVENTF_WHEEL 0x0800
#define WHEEL_DELTA 120
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
typedef struct {
    DWORD type;
    struct { INT dx,dy; DWORD mouseData,dwFlags; } mi;
} INPUT;
static INPUT last;
static HWND target;
static unsigned calls;
static void NtUserSendHardwareInput(HWND hwnd,int unused,INPUT *input,int extra)
{
    assert(!unused && !extra);
    last=*input;target=hwnd;calls++;
}
''' + helper + r'''
int main(void)
{
    int ticks[]={1,-1,3,-3,273,-273,274,-274,INT_MAX,INT_MIN};
    int expected[]={120,-120,360,-360,32760,-32760,32760,-32760,32760,-32760};
    for(unsigned i=0;i<sizeof(ticks)/sizeof(ticks[0]);i++) {
        memset(&last,0x55,sizeof(last));
        send_wheel(42,ticks[i]);
        assert(calls==i+1 && target==42 && last.type==INPUT_MOUSE);
        assert(last.mi.dwFlags==MOUSEEVENTF_WHEEL);
        assert((int32_t)last.mi.mouseData==expected[i]);
        assert(last.mi.dx==0 && last.mi.dy==0);
    }
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="pw-mouse-wheel-") as directory:
        source = Path(directory) / "wheel.c"
        executable = Path(directory) / "wheel"
        source.write_text(harness)
        cc = shlex.split(os.environ.get("CC", "cc"))
        flags = shlex.split(os.environ.get("CFLAGS", "-O2 -std=c11 -Wall -Wextra -Werror"))
        subprocess.run(cc + flags + [str(source), "-o", str(executable)], check=True, timeout=60)
        subprocess.run([str(executable)], check=True, timeout=10)
    print("PS5 mouse wheel: direction, scale, flags, cursor stability and bounds passed")


if __name__ == "__main__":
    main()
