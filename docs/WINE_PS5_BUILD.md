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
PS5_NATIVE_FOUNDATION=<pinned foundation> \
PS5_PRX_FOUNDATION=<foundation with module exports> tools/build_wine_ps5.sh
~~~

The payload SDK comes from `PS5_NATIVE_FOUNDATION` (the title's pinned
foundation, `.deps/ps5-native-app-boilerplate` by default) unless
`PS5_PAYLOAD_SDK` names one. `PS5_PRX_FOUNDATION` is described under
[PRX modules](#prx-modules).

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
| 0540 | `ntdll`: count host `mmap`/`munmap`/`mprotect`, resolved faults and per-image cost (`__wine_virtual_stats`, `WINEDEBUG=+module`); skip an `mprotect` that leaves every host page of its range unchanged (tracked host protection). Pinball start on the host: 1,810 → 749 `mprotect` with 16 KiB pages, → 730 with 4 KiB |
| 0550 | `ntdll`: `NtCreateUserProcess` and `__wine_unix_spawnvp` return `STATUS_NOT_SUPPORTED` on PS5: a title can neither fork nor exec (the prefix is initialised offline and the desktop is created in process) |

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

## PRX modules

The console does not load ELF shared objects, so after the ELF link the
script links `ntdll` and `win32u` again as PRX modules into
`.deps/wine-ps5/prx/sce_module/`:

1. Each module takes the objects of its ELF link, read back from
   `make.log`. `ntdll` adds the heap and the PS5 shims (`pw_wine_prx`,
   `pw_wine_dl`, `pw_wine_compat` and their libc bindings).
2. `tools/gen_prx_descriptor.py` adds the export descriptor and
   `module_start`. `ntdll` publishes `__wine_main`, `pw_wine_dl_adopt`,
   `pw_wine_heap_stats`, `dlopen` and `dlsym` for the title, and `win32u`
   publishes `__wine_unix_lib_init` for ntdll's `dlsym`. The descriptor's
   first constructor marks the module started, so constructors run once
   whether the firmware, the loader's entry call or `pw_wine_dl` runs
   `.init_array` first.
3. `prospero-lld --shared -Bsymbolic` links against the title's stub
   libraries, not the payload's static libc, using the foundation's
   `ps5-pie.ld` plus `wine/ps5/prx_eh_frame.ld`. The second script gives
   each module's libunwind its own hidden `__eh_frame*` bounds. `win32u`
   links against `ntdll.shared.elf`, so its 83 ntdll imports, all functions,
   become PRX-to-PRX function imports; a data import between application
   PRXs faults on the console.
4. `ps5-native-tool link --module` converts each module (using
   `ntdll.shared.elf` as the stub for `win32u`), then `self --sign` signs it.

Module conversion publishes exports only from foundation commit
`5bd0887e983abbf2f8a2eb762da8d4501b543179` onward. That commit is on the
foundation's `exp/prx-module` branch, and the title's pinned foundation
predates it. `PS5_PRX_FOUNDATION` (or `--prx-foundation`) names a checkout
that has it; otherwise the PRX link is skipped and the report says why.

`report.json` gains a `prx` section. For each module it lists whether the
PRX was built, what the stub link left unresolved, the tool's refusals, the
modules it needs, and its data imports.

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
console's libc, libkernel and SceNet stubs.

The PRX link of the same objects (host run on 2026-09-26, foundation
`1e9b564` for the tool):

| Module | Size | Unresolved | Needs | Data imports |
| --- | --- | --- | --- | --- |
| `ntdll.prx` | 654,537 bytes | none | `libSceLibcInternal`, `libkernel`, `libkernel_sys` | `__stderrp`, `__stdoutp` (libc), `environ` (libkernel) |
| `win32u.prx` | 2,217,250 bytes | none | `ntdll.prx`, `libSceLibcInternal`, `libkernel` | none |

- The data imports come from system modules the title already loads, not
  from another application PRX.
- The stubs have `isatty` only in `libScePosixForWebKit`, which a game title
  does not load. The compat layer provides it (no descriptor is a terminal),
  so ntdll needs only the libc and libkernel modules.
- Nothing in this note has run on the console.
