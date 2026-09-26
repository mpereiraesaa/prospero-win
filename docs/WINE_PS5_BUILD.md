# Wine's Unix side for PS5

Wine's PE modules already run through the IA-32 DBT and the WoW64 backend.
Its Unix side (`ntdll.so`, `win32u.so` and `wineserver`) is what runs Wine's
system services, and on the console it has to be native PS5 code. This note
records how it is built and what the console's libraries lack.

## Build

`tools/build_wine_ps5.sh` copies the pinned revision
(`490f6d5dcbb2a5047345b8af88d114bbcaad69a8`) into `.deps/wine-ps5/source`,
applies `wine/patches/*.patch` in numeric order and configures it out of
tree for `x86_64-unknown-freebsd11` with the PS5 payload SDK's
`prospero-clang`. The PS5 compiler defines `__FreeBSD__` and the SDK ships
FreeBSD headers, so Wine selects its FreeBSD paths (kqueue instead of epoll,
sysctl), and `__PROSPERO__` selects the PS5 patches. PE modules are not built
here; winebuild and widl come from the host build that
`tools/build_wine_runtime.sh` produces.

~~~sh
tools/build_wine_ps5.sh --check-patches   # validate and print the series
PROSPERO_WINE_SOURCE=<pinned checkout> PROSPERO_WINE_BUILD=<host build> \
PS5_PAYLOAD_SDK=<ps5-payload-sdk> tools/build_wine_ps5.sh
~~~

The link is made against the SDK's stub libraries with unresolved symbols
reported instead of fatal, so every run writes an exact list of what the
console does not provide to `.deps/wine-ps5/report.json`, per target, with
sizes, hashes and needed libraries. The SDK has no separate `libm`; its libc
carries the math functions, so the build supplies an empty `libm.a` for the
`-lm` that win32u requests, and links LLVM `libunwind` for
`_Unwind_Find_FDE`.

## Patch series

`wine/patches/NNNN-name.patch` files are mail-formatted patches applied with
`git apply` in numeric order. Numbers are owned by range so the two halves
of the port never collide:

| Range | Area |
| --- | --- |
| 0100–0499 | Unix services: unixlib loading as PRX, allocator, in-process wineserver transport, user driver, build |
| 0500–0899 | Execution core: signals, TEB/GS, virtual memory, process startup |

| Patch | Effect |
| --- | --- |
| 0100 | `ntdll`: a PS5 title has no fstab and no `getfsent`; report no default device |
| 0101 | `server`: resolve file names into server-owned memory instead of `realpath(path, NULL)` |
| 0102 | `server`: size the user shared data section to a whole host page; with 16 KiB pages that page also holds the syscall dispatcher pointer at `0x7ffe1000` (patch 0530) |
| 0500 | `ntdll`: signal context at `ucontext`+64 (measured); GS = TEB through `sysarch`; FS stays the libc TLS base, so the syscall dispatcher never switches it; no LDT for WoW64 threads |
| 0510 | `ntdll`: 16 KiB host pages under 4 KiB Windows pages, reusing the large-host-page path of `virtual.c` |
| 0520 | `ntdll`: name the ntdll directory with `WINE_PS5_NTDLL_DIR` when `dladdr` cannot (PRX) |
| 0530 | `ntdll`: with host pages larger than 4 KiB, store the x64 syscall-dispatcher pointer (0x7ffe1000) through the USD host page instead of mapping a separate page (needs the USD section sized to a host page) |

## Allocator

The title's libc `malloc` stops near 13 MiB, far below what Wine's Unix side
needs. `wine/ps5/pw_wine_heap.c` is a thread-safe heap over anonymous
mappings: power-of-two classes from 16 bytes to 64 KiB carved from 1 MiB
spans, and one mapping per larger block, unmapped on free. It keeps the live
and peak requested bytes, the mapped and peak mapped bytes, and counts of
allocations, frees, failures and foreign frees. Ownership is decided only
from its own sorted span and large-block registries, so a pointer it did not
return (for example one that libc's `strdup`, `realpath` or `getcwd`
allocated internally) is counted and left alone rather than read or freed.
The host test checks classes, reuse, large mappings, `realloc` in place and
across classes, zeroed `calloc`, overflow refusal, foreign and double frees,
and eight threads of mixed traffic under AddressSanitizer and
ThreadSanitizer.

`wine/ps5/pw_wine_heap_libc.c` binds `malloc`, `calloc`, `realloc`, `free`,
`strdup`, `strndup`, `asprintf` and `vasprintf` to that heap. The build links
it into `ntdll.so`, which exports it, so `win32u.so` imports the same heap
through `ntdll.so` and memory can cross between them; `wineserver` links its
own copy. The report records, per target, whether `malloc` is defined or
imported. Patch 0101 stops the server from receiving libc-allocated names:
`realpath(path, NULL)` becomes a resolve into a local buffer and a `strdup`.
The one-time `realpath(name, NULL)` in `ntdll`'s startup path is left as is
and appears as a foreign free.

## Measured result

With the series above, all three targets compile and link (host run on
2026-09-26):

| Target | `malloc` | Unresolved against the SDK |
| --- | --- | --- |
| `ntdll.so` | defines (heap) | none |
| `win32u.so` | imports from `ntdll.so` | none |
| `wineserver` | defines (own heap) | none |

Patch 0500 removed the last three unresolved symbols, the FreeBSD
`amd64_{get,set}_{fs,gs}base` wrappers: GS is set through `sysarch`, and FS is
never read or switched. Everything Wine's Unix side calls now exists in the
console's libc, libkernel and SceNet stubs. The
objects are linked as ordinary shared objects and an executable; turning
them into PRX modules (PRXDESC1 descriptors, dependency-ordered loading,
no cross-module data imports) is the next step and is not measured here.
Nothing in this note has run on the console.
