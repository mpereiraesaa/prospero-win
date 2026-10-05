#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run the CPU backend's end of a system or Unix call on a host.

Wine replaces the context of a thread suspended inside a call (the one it
read, with the guest's EAX from when it made the call) and flags
RESET_STATE; wow64 does the same where it means to (APC delivery, NtContinue,
exception dispatch, user callbacks). A Unix call's status must still reach
EAX: a thread that was only suspended otherwise returns its stale EAX. A
system call keeps the replaced context's EAX, as the backend always did:
Half-Life 2 stopped loading on the PS5 when its status replaced it.
"""
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
CPU = ROOT / "wine/wowprospero/cpu.c"
PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int32_t NTSTATUS;
typedef uint32_t DWORD;
#define WOW64_CPURESERVED_FLAG_RESET_STATE 1
#define PW_FP_IN_CONTEXT 0x8000
typedef struct { uint16_t Flags; uint16_t Machine; } WOW64_CPURESERVED;
typedef struct { DWORD ContextFlags, Eax, Ecx, Eip, Esp; } I386_CONTEXT;
'''
DRIVER = r'''
enum { SYSCALL = 0, UNIX_CALL = 1 };
int main(void)
{
    WOW64_CPURESERVED cpu = { 0, 0x14c };
    I386_CONTEXT ctx = { 0x1003f, 0x1234, 7, 0x401000, 0x12ff00 };

    /* Calls nothing interrupted: their status is EAX. */
    assert( service_return( &cpu, &ctx, 0x103, SYSCALL ) == 0 );
    assert( ctx.Eax == 0x103 && ctx.Eip == 0x401000 && ctx.Esp == 0x12ff00 && ctx.Ecx == 7 );
    assert( service_return( &cpu, &ctx, 0, UNIX_CALL ) == 0 && ctx.Eax == 0 && cpu.Flags == 0 );

    /* A Unix call suspended inside: Wine set back the context it read, whose
     * EAX is what the guest had when it called. The status replaces it. */
    ctx.Eax = 0x83c0d750;
    cpu.Flags = WOW64_CPURESERVED_FLAG_RESET_STATE;
    assert( service_return( &cpu, &ctx, 0, UNIX_CALL ) == 1 );
    assert( ctx.Eax == 0 && cpu.Flags == 0 );

    /* A Unix call whose context another thread replaced (SetThreadContext
     * moving EIP out of a patched range) keeps its other registers; the
     * backend's own flags are left alone. */
    ctx.Eip = 0x402000; ctx.Esp = 0x12fe00; ctx.Ecx = 9; ctx.Eax = 0xdeadbeef;
    cpu.Flags = WOW64_CPURESERVED_FLAG_RESET_STATE | PW_FP_IN_CONTEXT;
    assert( service_return( &cpu, &ctx, (NTSTATUS)0xc0000005, UNIX_CALL ) == 1 );
    assert( ctx.Eax == 0xc0000005 && ctx.Eip == 0x402000 && ctx.Esp == 0x12fe00 && ctx.Ecx == 9 );
    assert( cpu.Flags == PW_FP_IN_CONTEXT );

    /* A system call that came back through a replaced context without any
     * suspension: an alertable wait that ran a user APC (NtContinue to the
     * context saved at delivery), an exception dispatched from inside the
     * call (EIP at the dispatcher), a user callback. The context stands as
     * wow64 left it, EAX included, and the flag is consumed. */
    ctx = (I386_CONTEXT){ 0x1003f, 0x4, 3, 0x401000, 0x12ff00 };
    cpu.Flags = WOW64_CPURESERVED_FLAG_RESET_STATE;
    assert( service_return( &cpu, &ctx, 0xc0, SYSCALL ) == 1 );
    assert( ctx.Eax == 0x4 && ctx.Eip == 0x401000 && ctx.Esp == 0x12ff00 && cpu.Flags == 0 );
    ctx.Eip = 0x7bc01000; ctx.Esp = 0x12fc00;
    cpu.Flags = WOW64_CPURESERVED_FLAG_RESET_STATE | PW_FP_IN_CONTEXT;
    assert( service_return( &cpu, &ctx, 0, SYSCALL ) == 1 );
    assert( ctx.Eax == 0x4 && ctx.Eip == 0x7bc01000 && ctx.Esp == 0x12fc00 && cpu.Flags == PW_FP_IN_CONTEXT );

    /* The next call, not replaced, returns its status again. */
    assert( service_return( &cpu, &ctx, 0x102, SYSCALL ) == 0 && ctx.Eax == 0x102 );
    puts( "ok" );
    return 0;
}
'''


def helper_source(source: str) -> str:
    match = re.search(r"^static int service_return\(.*?^\}\n", source, re.S | re.M)
    if not match:
        raise AssertionError("cpu.c has no service_return()")
    return match.group(0)


class ServiceReturn(unittest.TestCase):
    def test_status_and_replaced_contexts(self):
        compiler_command = shlex.split(os.environ.get("CC", "cc"))
        compiler = shutil.which(compiler_command[0])
        self.assertIsNotNone(compiler, "a C compiler is required")
        helper = helper_source(CPU.read_text())
        with tempfile.TemporaryDirectory(prefix="pw-service-return-") as directory:
            path = pathlib.Path(directory)
            (path / "test.c").write_text(PRELUDE + helper + DRIVER)
            flags = shlex.split(os.environ.get("CFLAGS", "-std=c11 -O1 -Wall -Wextra -Werror"))
            subprocess.run([compiler, *compiler_command[1:], *flags, str(path / "test.c"),
                            "-o", str(path / "test")], check=True, timeout=30)
            subprocess.run([str(path / "test")], check=True, timeout=5, stdout=subprocess.DEVNULL)

    def test_both_calls_end_through_it(self):
        source = CPU.read_text()
        simulate = source[source.index("void WINAPI BTCpuSimulate"):]
        for reason, call, kind in (("PW_WOW_SYSCALL", "Wow64SystemServiceEx", 0),
                                   ("PW_WOW_UNIXCALL", "unix_call_dispatcher", 1)):
            case = simulate[simulate.index(f"case {reason}:"):]
            case = case[:case.index("break;")]
            with self.subTest(reason=reason):
                self.assertRegex(case, rf"status = {call}\(.*\);\s*if \(service_return\( cpu, ctx, status, {kind} \)\)")
        self.assertEqual(simulate.count("ctx->Eax = status"), 0)


if __name__ == "__main__":
    unittest.main()
