# Native PE64 bootstrap in a WoW64 process

Patch 0902 lets a 32-bit game process run native 64-bit service threads next to
its WoW64 threads. It was written for an experimental 64-bit D3D9 backend that has
since been removed. The patch stays because the callback, session, suspension and
IME fixes described in the related notes build on it: [callback
routing](wine-native-callback-domain.md), [session tokens](wine-native-session.md),
[suspension](wine-native-suspend.md). Nothing in the runtime starts a native
service thread on its own; only the lab fixtures below do.

## Thread and loader contract

Patch 0902 adds a private, explicit native service domain. The ordinary PE32
`NtCreateThreadEx` thunk rejects its private flag. A native-domain service's
`CreateThread` descendants inherit the domain in Unix thread data, before TEB
and stack allocation. Their TEB64 has no WoW TEB; `get_cpu_area` returns no guest
context. Ordinary guest threads retain the existing allocation/startup path.
On Linux the native thread still records the pthread FS base needed by signal
entry, without allocating a guest FS selector.

Wine's native loader in a WoW64 process normally exits into `Wow64LdrpInitialize`
before native kernel32, locale state, and the DLL attach graph are initialized.
The service path initializes these under the native loader lock, allocates TLS
and FLS, and performs native DLL thread notifications. Native exception dispatch
skips the guest CPU reset hook. Native detach skips the PE32 main image's TLS
callbacks. The service uses Wine's ordinary thread shutdown and wait machinery.
There is no raw-pthread call into Windows code and no process-wide WoW64 disable.

## Private bootstrap v1

A PE32 caller invokes `NtQueryInformationProcess(NtCurrentProcess(), 0x50570001,
request, 592, &length)`. This private operation is synchronous and deliberately
limited to the current process. Its fixed layout is:

| Offset | Field |
| --- | --- |
| 0 | `uint32_t version`, exactly 1 |
| 4 | `uint32_t size`, exactly 592 |
| 8 | 260 UTF-16 code units: an absolute drive path, with final code unit zero |
| 528 | Eight `uint64_t` result values, initially zero |

The thunk copies the request into native-owned memory, creates a Wine thread,
loads the exact path, resolves `PwD3D9ServiceMain`, and invokes it with the result
array. The entry has `DWORD WINAPI (uint64_t result[8])` calling convention. The
module must join all its children before returning. Its DLL is unloaded only
after the entry returns; the thunk waits for thread termination before copying
results and freeing the request. Load, symbol-resolution, and entry failures
are returned to the client, with no backend substitution. This is a bootstrap
probe ABI.

## Reproduce the host proof

Build the pinned Wine revision with the project patch series, then run:

```sh
python3 tests/lab/wine_native_domain.py \
  --wine-build /path/to/patched/wine/build \
  --prefix /path/to/disposable/wine-prefix \
  --output /tmp/native-domain-proof \
  --dxvk64 /path/to/pinned/dxvk/x64/d3d9.dll
```

The runner compiles the same fixture as a PE32 executable and a PE64 DLL,
records hashes, and requires ten successful load/unload cycles for each mode.
The optional `--baseline-wine /path/to/original/loader/wine` verifies that the
unmodified runtime rejects the extension. The output directory must be new.

The fixture checks native service and child TEBs, Win32 and CRT TLS isolation,
a read/write allocation constrained above 4 GiB, vectored exception handling on both native
threads, ordinary PE32 child startup before and after, and malformed request
version/length rejection, and a missing-DLL failure followed by successful bootstrap. With `--dxvk64`, it loads the specified PE64 DXVK
DLL, calls `Direct3DCreate9`, and releases the returned real interface on every
iteration. It never substitutes Wine D3D9 or a 32-bit DLL.

On 2026-10-09 the host proof passed ten native and ten DXVK cycles. The tested
backend was the pinned `2.6.2-prospero1` x64 DLL, SHA-256
`1d626ff743d93f78e1369c1f28daf5bb3b7c0832eef2876a6f352ec48f575710`.
Ordinary DXVK factory allocations were still below 4 GiB (for example,
`0x1350c70`). The explicit high allocation proves addressability; it does not
prove that ordinary native heaps have moved out of guest address space. A later
memory policy and low-address usage measurement are required for that claim.

The native import closure included kernelbase, user32, gdi32, setupapi, and
winevulkan; the factory enumerated the host NVIDIA adapter. This establishes
factory creation and teardown only. Device creation, guest/service window
association, rendering, and performance remain unproven.

Console checks must use the designated operator with an immutable package,
matching ntdll PRX/PE and wow64 DLLs, the PE32 fixture, PE64 service, and pinned
backend. Capture `PW_NATIVE_DOMAIN` records and restore the original runtime
and title configuration. Do not run a game using this probe entry point.

The first console probe correctly loaded the native service, then returned its
allocation failure code: the fixture had required exactly 8 GiB, outside this
port's reserved native region starting at 64 GiB. The fixture now requests
`VirtualAlloc2` with a 4 GiB lower bound and no fixed base, then verifies the
actual returned address and data. It also requires an inverted address range
to fail with `ERROR_INVALID_PARAMETER`, so ignored constraints cannot pass.
This preserves the high-address requirement while allowing the platform
allocator to choose available storage.

## Console proof

The corrected probe passed on 2026-10-09 with native CPU and a 120 Hz profile.
Independent saved-log review found 281 native records and 845 DXVK records,
with no sequence gaps and explicit `session_end reason=wine-exit` in both:

| Probe | Successful iterations | Result flags | Expected exceptions |
| --- | --- | --- | --- |
| Native service/child | 10 of 10 | 31 | 20 |
| PE64 DXVK factory | 10 of 10 | 63 | 20 |

Native allocations were `0x1000360000` through `0x10006b0000`. Unlike the host
run, the console's ordinary DXVK factory objects were also above 4 GiB:
`0x1000110c70` through `0x10005f55e0`. This is evidence for those observed
objects, not a claim about all backend allocations or total memory pressure.
No D3D device or game was run. The service and child each handled the fixture's
expected exception in PE64 context (`cs=0043`).

Log SHA-256: native
`12c7f28eefe2de7238e7759ceeacd1b4aac4efd3c3bb82f2837cf578590ebc92`;
DXVK `b7f10fcf2c74563fa167caf31bd26311c483a1ab123dffdeeedde282b39cb19a`.
The operator restored the original runtime, removed temporary probe files and
profiles, restored the normal title executable, and confirmed no title running.
The runtime code was unchanged between the failed fixed-address probe and this
pass; only the synthetic allocation test and deployment-root paths changed.
