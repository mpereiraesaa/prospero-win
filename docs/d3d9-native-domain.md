# Native PE64 bootstrap in a WoW64 process

This is the first implementation gate of the [approved bridge design](d3d9-bridge-design.md).
It does not implement a D3D9 proxy, device transport, presentation windows, or game support.

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
probe ABI; a persistent service and bounded transport remain subsequent work.

## Reproduce the host proof

Build the pinned Wine revision with the project patch series, then run:

```sh
python3 tests/lab/d3d9_native_domain.py \
  --wine-build /path/to/patched/wine/build \
  --prefix /path/to/disposable/wine-prefix \
  --output /tmp/native-domain-proof \
  --backend64 /path/to/pinned/dxvk/x64/d3d9.dll
```

The runner compiles the same fixture as a PE32 executable and a PE64 DLL,
records hashes, and requires ten successful load/unload cycles for each mode.
The optional `--baseline-wine /path/to/original/loader/wine` verifies that the
unmodified runtime rejects the extension. The output directory must be new.

The fixture checks native service and child TEBs, Win32 and CRT TLS isolation,
a read/write allocation at 8 GiB, vectored exception handling on both native
threads, ordinary PE32 child startup before and after, and malformed request
version/length rejection, and a missing-DLL failure followed by successful bootstrap. With `--backend64`, it loads the specified PE64 DXVK
DLL, calls `Direct3DCreate9`, and releases the returned real interface on every
iteration. It never substitutes Wine D3D9 or a 32-bit DLL.

On 2026-10-09 the host proof passed ten native and ten DXVK cycles. The tested
backend was the pinned `2.6.2-prospero1` x64 DLL, SHA-256
`1d626ff743d93f78e1369c1f28daf5bb3b7c0832eef2876a6f352ec48f575710`.
The native import closure included kernelbase, user32, gdi32, setupapi, and
winevulkan; the factory enumerated the host NVIDIA adapter. This establishes
factory creation and teardown only. Device creation, guest/service window
association, rendering, console startup, and performance remain unproven.

Console checks must use the designated operator with an immutable package,
matching ntdll PRX/PE and wow64 DLLs, the PE32 fixture, PE64 service, and pinned
backend. Capture `PW_NATIVE_DOMAIN` records and restore the original runtime
and title configuration. Do not run a game using this probe entry point.
