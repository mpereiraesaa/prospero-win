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
| 0110 | `server`: run in-process (`WINE_INPROCESS_SERVER`, set on PS5): `pw_wineserver_connect()` starts the server on a thread and returns a client socket; see [In-process server](#in-process-server) |
| 0111 | `ntdll`: connect through `pw_wineserver_connect()` from `wineserver.so` beside ntdll (`wineserver.prx` on PS5) instead of the socket file, and register each thread's kernel id with the server module |
| 0400 | `win32u`: in-process PS5 user driver (`WINE_PS5_USER_DRIVER`, set on PS5); see [User driver](#user-driver) |
| 0500 | `ntdll`: signal context at `ucontext`+64 (measured); GS = TEB through `sysarch`; FS stays the libc TLS base, so the syscall dispatcher never switches it; no LDT for WoW64 threads |
| 0510 | `ntdll`: 16 KiB host pages under 4 KiB Windows pages, reusing the large-host-page path of `virtual.c` |
| 0520 | `ntdll`: name the ntdll directory with `WINE_PS5_NTDLL_DIR` when `dladdr` cannot (PRX) |
| 0530 | `ntdll`: with host pages larger than 4 KiB, store the x64 syscall-dispatcher pointer (0x7ffe1000) through the USD host page instead of mapping a separate page (needs the USD section sized to a host page) |
| 0540 | `ntdll`: count host `mmap`/`munmap`/`mprotect`, resolved faults and per-image cost (`__wine_virtual_stats`, `WINEDEBUG=+module`); skip an `mprotect` that leaves every host page of its range unchanged (tracked host protection). Pinball start on the host: 1,810 → 749 `mprotect` with 16 KiB pages, → 730 with 4 KiB |
| 0550 | `ntdll`: `NtCreateUserProcess` and `__wine_unix_spawnvp` return `STATUS_NOT_SUPPORTED` on PS5: a title can neither fork nor exec (the prefix is initialised offline and the desktop is created in process) |
| 0560 | `ntdll`: all Unix-side stderr goes through a sink the title sets with `__wine_ps5_set_output_sink` (a title cannot give Wine a usable fd 2); `fatal_error` formats into a local buffer |
| 0570 | `ntdll`: reserve address space with a fixed, no-overwrite `sceKernelReserveVirtualRange`, since `MAP_FIXED \| MAP_EXCL` replaces existing mappings on FW 12.02; the view heap gets 64 MiB above 4 GiB |
| 0580 | `ntdll`: skip the configuration directory's parent ownership check on PS5, as 0111 skips the one after `chdir` |

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

Module conversion publishes exports from foundation commit `5bd0887`
onward, and binds imports between application PRXs from
`30597512539e7edfde079cbcaf4a626bc0a948c5` (the name-form export hash) onward. The
build requires the second. Both are on the foundation's `exp/prx-module`
branch, and the title's pinned foundation predates them. `PS5_PRX_FOUNDATION` (or `--prx-foundation`) names a checkout
that has it; otherwise the PRX link is skipped and the report says why.

`report.json` gains a `prx` section. For each module it lists whether the
PRX was built, what the stub link left unresolved, the tool's refusals, the
modules it needs, and its data imports.

## In-process server

A PS5 title cannot create processes, so wineserver runs on a thread of the
process that hosts Wine. Patch 0110 adds `pw_wineserver_connect(nls_dir)`:

1. On first use, the caller's thread runs the server's initialisation (what
   `main()` does). It skips option parsing, signal handlers, rlimits, the
   lock file and the fork into a daemon.
2. `main_loop()` then runs on its own thread.
3. Each call returns the client end of a new socketpair. The server end
   reaches the server thread through a channel with `SCM_RIGHTS` and is
   accepted as a new process, exactly like a connection on the master
   socket.

Other differences from a standalone server:
- The NLS directory comes from the caller, and the server directory lives
  under the prefix.
- When idle, the server flushes the registry but keeps running and keeps
  the channel open, so the host can start Wine again.
- It never signals its own host process.

`tools/test_wine_inprocess_server.sh` builds the patched server objects for
the host with `WINE_INPROCESS_SERVER` and links them into
`tests/wine_inprocess_server_check.c`. The test starts the server inside the
test process and receives the protocol version and request pipe over two
connections, dropping the first one before it initialises. It passes on the
host with protocol 961. It needs the host Wine build, so it is not part of
`make test`.

On the ntdll side, patch 0111 keeps `server_connect()`'s configuration
directory setup. Instead of the socket file and `start_server()`, it:

1. `dlopen`s `wineserver.so` from ntdll's directory (`server/` in a build
   tree). On PS5, `pw_wine_dl` loads `wineserver.prx` for it.
2. Calls `pw_wineserver_connect()` with ntdll's NLS directory.
3. Before each thread's first request, registers the thread's kernel id
   with `pw_wine_thread_register`.

The title libraries have no `thr_kill2`, `thr_kill` or `syscall`, so
`wineserver.prx` signals threads with `pthread_kill` through that registry
(`wine/ps5/pw_wine_threads.c`).

A full host run exercised both patches:
- Host Wine was built with `WINE_INPROCESS_SERVER` for ntdll and the
  server, with the server objects linked into `server/wineserver.so`.
- The prefix was initialised by an ordinary build.
- `reg query "HKLM\Software\Microsoft\Windows NT\CurrentVersion" /v
  CurrentVersion` then printed `REG_SZ 6.3` and exited 0.
- The `+server` trace shows `connected to the in-process server`, and no
  `wineserver` process existed during the run.
- Helper processes that Wine spawns on Linux (explorer, winedevice) each
  get a server of their own and fail as expected; on PS5, patch 0550 stops
  process creation.
- `pw_wineserver_connect` is `DECLSPEC_EXPORT`, because Wine compiles its
  unix code with `-fvisibility=hidden`.

The PRX stage links `wineserver.prx` from the server objects:
- its own heap copy;
- the compat shims, whose `posix_fadvise` and `if_nametoindex` are needed
  only by the server;
- the thread registry;
- a descriptor with `pw_wineserver_connect` and `pw_wine_thread_register`.

## User driver

A title has no explorer, no display server and no driver dll, so patch 0400
gives win32u its own driver (`dlls/win32u/ps5drv.c`). `load_display_driver()`
installs it, and `get_desktop_window()` takes the desktop the server creates
instead of starting explorer. The driver:

- **Display.** It reports one virtual monitor: 800x600, or `WINE_PS5_DESKTOP=WxH`.
  It sizes the server's desktop window to that monitor and publishes the
  monitor once, so the server bounds the cursor by it even when the prefix
  holds another driver's display config.
- **Presentation.** Each window gets a 32-bpp surface. On flush, the pixels
  of visible top-level windows are composed into one screen buffer and
  handed to `pw_wine_present()`, the title's single sink
  (`wine/ps5/pw_wine_sink.c` in ntdll.prx). A window that has just been
  shown is redrawn, since it may have painted before it had this surface.
- **Input.** `ProcessEvents` drains `pw_wine_next_input()` into hardware
  input:
  - keys;
  - the absolute pointer, in screen pixels;
  - buttons.

  `pw_wine_input_fd()` becomes each GUI thread's queue fd, so an idle
  thread is woken when input is posted. When no window is in the
  foreground, the newest shown window is brought there, because no window
  manager does that here.

Host check (2026-09-26): a host Wine was built with `WINE_INPROCESS_SERVER`
and `WINE_PS5_USER_DRIVER`, with `tests/wine_ps5_driver_sink.c` preloaded as
the sink. The fixture writes the latest frame as a PPM and replays scripted
input.
- winemine and notepad appear in the 800x600 frames, with caption, borders,
  board and edit area.
- A scripted click at (40,200) opened winemine's board and started its timer.
- Typed `HI` reached notepad's edit control as `WM_KEYDOWN`/`WM_CHAR` (`h`, `i`).
- No text is drawn, because the build has no fonts (`--without-freetype`,
  as on PS5). Fonts for the console are still open.

Patch 0410 fixes two window-size errors that this setup causes:

- **Menu height without fonts.** `NtGdiGetTextMetricsW` fails when there
  are no fonts, and `get_text_metr_size()` left the caller's `TEXTMETRICW`
  uninitialised. The menu height (`SM_CYMENU`) became stack garbage
  (6,750,319). A font-less `wineboot` also stored it in the prefix as
  `MenuHeight`, so a prefix initialised before 0410 needs that value
  removed from `user.reg`. The metrics are now zeroed first.
- **Desktop rectangle.** With this driver the server creates the desktop
  window for the process, and the process holds no `WND` for it.
  `GetWindowRect(GetDesktopWindow())` failed and left the caller's
  rectangle uninitialised. The desktop is now treated as `WND_DESKTOP`.

Pinball sizes and centres its window from these values. Before 0410 it
placed the window at (-303,-32768), so the window was off-screen: it
painted once and never again. With 0410, on a host WoW64 build with this
driver and a preloaded sink:

- the window is at (97,44)-(703,511);
- the table is presented continuously;
- Alt+F4 posted through the input queue closes the game.

## WoW64 CPU backend

The PRX stage also links `wowprospero.prx`, the Unix side of the WoW64 CPU
backend (`wine/wowprospero`). It holds `unix.c` and the IA-32 DBT sources,
built with the payload SDK, and imports ntdll's functions like win32u does.
Its descriptor exports `__wine_unix_call_funcs`, which ntdll's `dlsym`
looks up.

`unix.c` keeps one `__thread` pointer, which the PS5 compiler turns into
emulated TLS, so the module also links the payload SDK's own `emutls.o`.
Host run: no unresolved symbols, nothing bound only to `libkernel_sys`, no
raw syscalls, 142,235 bytes.

To use it on the console:
- the PE side, `wowprospero.dll` from `tools/build_wowprospero.sh`, goes in
  `lib/wine/x86_64-windows` and in the prefix's `system32`;
- the prefix selects it with
  `HKLM\Software\Microsoft\Wow64\x86` (default value) =
  `wowprospero.dll`, since Wine's default `wow64cpu.dll` needs 32-bit
  compatibility mode, which the console refuses.

## Imports a title does not get

The SDK stubs include `libkernel_sys`, so a PRX links against functions
only that library exports. A game title does not get `libkernel_sys`: the
firmware leaves those imports at 0, and a call to one jumps to address 0.

This was measured on FW 12.02 (log `20260926T150634522Z`). A diagnostic gate
loaded `wineserver.prx` through ntdll's `dlopen` and read all 127 of its
import slots. Exactly four stayed 0: `fchdir`, `link`, `fstatfs` and
`ptrace`, the four that only `libkernel_sys` exports. That is the `rip=0`
fault in the first console start of the in-process server.

The compat layer now provides these:
- `fstatfs` fails with `ENOSYS`;
- `link` and `ptrace` fail with `EPERM`;
- `umask` is remembered.

The report lists, per module, the imports still bound only to
`libkernel_sys` (`title_unbound`), and the build warns about them. The
remaining ones are ntdll's `fchdir`, `fstatat`, `openat` and `symlink`, and
wineserver's `fchdir`. They belong to the virtual working directory
(`pw_wine_cwd`), which must emulate them with libkernel calls; once it does,
the warning becomes a failure.

## Data directory

A title can write only its own `/download0` sandbox, where `/data` is absent,
so the Wine prefix would be limited to that sandbox. Before starting Wine,
prospero-win (`native/pw_data_mount.c`) writes a request file,
`/download0/etahen_jailbreak`, carrying its process id, then waits until
`/data` becomes reachable. This expects the helper daemon from
<https://github.com/ArkSama/PS5-Lapy-JB-Daemon> to be running on the console:
it detects the request file and makes `/data` available to the process, after
which the prefix lives at `/data/prospero-win/prefix`. The request is
best-effort — if the helper is not running, the title keeps using
`/download0/prospero-win/prefix`.

## Starting Wine in the title

`PW_NATIVE_MODE=wine64` builds the title around `native/wine64_main.c`.
The title runs one game per process (`src/pw_wine_launch.h`):

- **Launcher.** Started with no game (as the system starts it), the title
  shows the launcher (`src/pw_launcher_render.c`) on VideoOut without
  loading Wine. The d-pad moves the selection. Cross restarts the title with
  `sceSystemServiceLoadExec` and the game's arguments
  (`profile=pinball path=C:\Games\Pinball\PINBALL.EXE cycle=<n>`). A
  restart is a new process that receives its arguments intact in about
  430 ms (see `HARDWARE_VALIDATION.md`).
- **Game.** Started with `profile=` or `path=`, the title runs that executable
  in Wine as below.
- **Closing.** Holding Options+Create for a second sends the game Alt+F4. When
  Wine exits, the title restarts into the launcher. If the game has not
  closed after 5 s, the title restarts into the launcher anyway.
- **Unattended validation.** `-DPW_WINE64_SCRIPT=1` makes the launcher open
  the first game by itself, `PW_WINE64_SCRIPT_CYCLES` times.
  `-DPW_WINE64_SECONDS=<s>` closes each game after that long; the default, 0,
  lets a game run until it is closed.

In a game, the title works as follows:

1. It requests the `/data` mount (`native/pw_data_mount.c`) and uses
   `/data/prospero-win/prefix`, or `/download0/prospero-win/prefix` when
   `/data` does not appear.
2. It loads `ntdll.prx` from `/app0/win/wine/lib/wine/x86_64-unix`.
3. It registers ntdll's stderr sink (patch 0560), which turns Wine's debug
   channels into `WINE ...` ps5log lines.
4. It sets ntdll's present sink (`pw_wine_set_present_sink`), opens VideoOut
   and the DualSense, and maps two frame buffers.
5. It enters `__wine_main` for the chosen executable on its own
   thread (`src/pw_wine_start.c`).

The present sink runs on Wine's threads and only copies the frame into a
`PwWineFrameBox` (`native/pw_wine_display.c`). The main thread shows the newest
frame through `pw_videoout_ps5_present`, which scales it into the 1920x1080
scanout and waits for the vblank. The same thread reads the pad and posts
Pinball's keys with `pw_wine_post_input`: L1 and R1 flip, Cross plunges, the
d-pad nudges, Options pauses and Square starts a new game, the same map the
direct runtime uses. Frames up to 1280x1024 are shown; larger ones are
counted as rejected.

Until Wine installs its own handlers, a fault is reported with its RIP
(read at ucontext +224) and the ntdll segment it falls in. Once a second
the main thread logs a heartbeat with the frames put and shown and the
inputs posted, and every five seconds ntdll's address-space counters, for
as long as the game runs. Pinball's first frame arrives about 31 s in.
`WINEDEBUG` defaults to `err+all,+loaddll,+process`; `+seh` is left out
because WoW64 callback returns unwind with `80000026` many times a second.

The runtime is found at `/app0` or, failing that, at
`/mnt/sandbox/PPSA99995_000/app0`, whichever holds
`win/wine/lib/wine/x86_64-unix/ntdll.prx`. Once `/data` is granted, the
process sees the real root, where `/app0` does not exist.

The runtime is staged beside the title:
- `ntdll.prx`, `win32u.prx` and `wineserver.prx` under
  `win/wine/lib/wine/x86_64-unix`;
- Wine's NLS files under `win/wine/share/wine/nls`.

## Console bring-up

The runtime and the prefix used for the integrated runs:

- **Runtime (temporary, removed after each run).** 1,684 files, 515 MiB,
  uploaded in 67 s:
  - the three PRXs;
  - the debug-stripped x86_64 and i386 PE modules of a pinned WoW64 host
    build (`--enable-archs=i386,x86_64`);
  - the NLS files.
- **Prefix (Wine's persistent state at `/data/prospero-win/prefix`).**
  - Initialised on the host with the same build (`wineboot --init`), because
    patch 0550 stops wineboot on the console.
  - Its `system32`, `syswow64` and `winsxs` modules are stripped. Wine needs
    both these copies and the ones in `lib/wine`: without either,
    `kernel32.dll` or `start.exe` fails to load.
  - Pinball is at `drive_c/Games/Pinball/PINBALL.EXE`.
  - `dosdevices` is left out, so Wine creates it on the console.

Run 2 (FW 12.02, 2026-09-26, ps5log `20260926T161550691Z` and klog):

1. `/data` was granted after 100 ms, and ntdll.prx loaded from the sandbox
   view of `/app0`.
2. The stderr sink carried Wine's startup through `init_paths`,
   `virtual_init`, `init_environment`, `start_main_thread`,
   `virtual_alloc_first_teb`, `dbg_init` and into `server_init_process`.
3. The kernel then killed the title. klog:
   `the process pid=2681 directly issued a syscall 57` and
   `exception: 0xa002030a (SYSTEM_ILLEGAL_FUNCTION_CALL)`.

   Syscall 57 is `symlink`, issued by the working-directory shim while
   `setup_config_dir` created `dosdevices/c:`. A title may not execute a
   `syscall` instruction outside libkernel. The report now lists such
   instructions per module (`raw_syscalls`).

   Wine's own dispatchers keep one on the FS-base restore path, which patch
   0500 never takes, and they are allowed. The shim's `link`, `readlink`,
   `statfs`, `symlink` and `symlinkat` wrappers remain and are a warning
   until they are emulated; imports that bind only to `libkernel_sys` are
   now fatal.

Runs 3–7 (FW 12.02, 2026-09-26; the baseline eboot `d9361fcc` was restored
and the temporary runtime removed after each):

- **Virtual symbolic links** (#91). A prefix whose `dosdevices/` already
  exists gets `dosdevices/.pw-symlinks` with `c:` and `z:`.
- **Server connection.** `server_init_process` connects to the in-process
  server (`connected to the in-process server …/wineserver.so`).
  - A title has no `pipe()`, so the first client thread got no request pipe
    and Wine exited with 1 (klog `exit_value=1`).
  - The compat layer now makes `pipe()` a socket pair (#92), and the run
    reaches `server connected`, i.e. `server_init_process_done`.
  - `wineboot` fails cleanly (`c00000bb`, patch 0550).
- **fd 2 capture.** The title captures fd 2 with a socket pair, so the
  in-process server's own output appears as `WINESERVER …` records.
- **Main executable.** Without `WINEARCH=wow64`, Wine starts the i386
  `PINBALL.EXE` from `start.exe` in a new process, which the title refuses
  (`process creation is not supported on PS5`). With it (set by the title):
  - `PINBALL.EXE` is mapped at `0x1000000` in-process;
  - Wine's x86_64 PE code runs on the console: ntdll, kernelbase, kernel32,
    msvcrt, ucrtbase, advapi32, win32u, user32, gdi32, shell32 and imm32
    load and attach;
  - `wow64.dll` and `wow64win.dll` load.
- **Where it stops.** Run 7 (`+seh`, ps5log `20260926T173846694Z`) shows
  68,786 `handle_syscall_fault c0000005`:
  - 68,573 at `0x80003dd4b` and 212 at `0x800041570`, both inside
    libkernel, reading address `0x10`;
  - each returns to PE `__wine_dbg_output`.

  Unix code reached from a PE system call calls into libkernel (the title's
  output sink) and finds its thread pointer invalid. Each fault is traced,
  which faults again, until `virtual_setup_exception stack overflow` and
  exit 1. The next step is the thread's segment bases while Wine's Unix side
  runs (GS = TEB, patch 0500), before `wowprospero` can start `PINBALL.EXE`'s
  i386 code.

Runs 8–10 (FW 12.02, 2026-09-26; baseline restored and runtime removed after
each):

- **Run 8: null FS selector** (ps5log `20260926T175244919Z`, local debug
  patch only).
  - `sysarch(AMD64_GET_FSBASE)` returned the thread's FS base both in the
    fault handler and at the next Unix call, which then faulted on
    `fs:0x10`. `GET_FSBASE` reports the kernel's saved value, not the
    hardware base.
  - The saved context of the first fault has `mc_fs = 0x13`. Every later
    fault has `mc_fs = 0`: after the first signal return the FS selector is
    null, so the hardware FS base is 0.
  - Patch 0590 (#94) repairs a null `%fs` on each entry to Unix code by
    calling libkernel's `sysarch(AMD64_SET_FSBASE)`.
- **Converter binding.** The first fault was a call through `win32u`'s
  `NtCurrentTeb` import, which was still 0 (return address
  `win32u+0x104b0e`).
  - All 86 of `win32u.prx`'s imports from `ntdll.prx` stayed 0, while its
    libc and libkernel imports were bound.
  - The firmware looks exports up by the hash of
    `NID#<library name>#<module name>`, but `link --module` hashed the
    one-letter form `NID#C#A`. A lookup only succeeded when both landed in
    the same bucket.
  - Hashing the name form for module imports and exports fixes it: with
    the fixed converter, all 129 of `win32u.prx`'s import slots are bound.
    The fix is foundation commit `3059751` on `exp/prx-module`, which the
    build now requires.
- **Run 9** (ps5log `20260926T183138315Z`, #94 and the fixed converter).
  - No system call faults (run 7 had 68,786).
  - `win32u`'s `__wine_unix_lib_init` runs, and `wow64.dll`, `win32u.dll`
    and `wow64win.dll` load.
  - WoW64 then loads `wow64cpu.dll` instead of `wowprospero.dll`, switches
    to 32-bit mode and overflows its stack in `wow64cpu`.
- **Run 10: empty registry** (`+reg`, ps5log `20260926T183540386Z`).
  - `NtOpenKeyEx` fails for `\Registry\Machine\Software\Microsoft\Wow64\x86`,
    `Session Manager\Environment` and `ProfileList`, although the prefix's
    `system.reg` has `@="wowprospero.dll"` under `Wow64\x86`.
  - The in-process server loads its registry with `fchdir(config_dir_fd)`
    and then `fopen("system.reg")`. The working-directory shim does not wrap
    `fopen`, so the relative open uses the process's real directory and
    fails silently.
  - With no `Wow64\x86` value, WoW64 uses its default CPU, `wow64cpu.dll`.
    The shim now resolves `fopen` through the working directory (#96).

Runs 11–12 (FW 12.02, 2026-09-26, main at `a509f5e` with the fixed
converter; baseline restored and runtime removed after each):

- **Run 11** (`+seh`, 30 s, ps5log `20260926T184446169Z`).
  - The registry loads, and WoW64 takes its CPU from it: `wowprospero.dll`
    loads after `wow64.dll`, and `PINBALL.EXE` is mapped at `0x1000000`.
  - `PINBALL.EXE`'s i386 code runs through `wowprospero`: the i386 ntdll
    loads its imports, from `kernel32.dll` to `winmm.dll`, `imm32.dll` and
    `uxtheme.dll`.
  - The 67 `RtlUnwindEx code=80000026` records are
    `STATUS_UNWIND_CONSOLIDATE` unwinds into `wow64.dll`, WoW64's normal
    return from a user callback, not faults.
  - The title's heartbeat reported `faults=0` throughout, and the run ended
    at the title's deadline with `status=0`.
- **Run 12** (`+loaddll,+process`, 120 s, ps5log `20260926T184852813Z`).
  - By 33 s, `PINBALL.EXE` has loaded `oleaut32.dll`, `version.dll` and
    `mmdevapi.dll`, found no audio driver
    (`No driver from L"pulse,alsa,oss,coreaudio"`), and failed to open its
    music (`Couldn't load driver for type L"PINBALL.MID"`).
  - From 35 s to the 120 s deadline, the mapping counters and output stop
    changing (`mmap=139 mprotect=448 images=34`), with `faults=0`. The game
    is waiting in its message loop.
  - Other errors: `Wine was built without Vulkan support` and
    `wgl: Failed to create internal thread context`.

What the integrated runs establish:

- **Signals.** Runs 7–8 delivered 68,786 SIGSEGVs from Unix code as
  `c0000005` exceptions. Their fault addresses and the RIP read from the
  context at `ucontext+224` matched the libkernel disassembly. Runs 11–12
  took no faults.
- **Per-thread TEB.** GS holds the TEB, and patch 0590 restores FS on each
  Unix entry. With both, Wine's x86_64 and i386 PE code ran for 120 s on
  thread `0024` with no segment fault.

The wine64 title does not yet install the present sink or input source, so
the game's window is not shown. The next step is connecting the user
driver's present sink and input to the title, plus an audio driver.

Runs 13–15 (FW 12.02, 2026-09-27; baseline restored and runtime removed
after each) use the title's present sink and pad input (#98, #99) and the
launcher (#104):

- **Run 13** (main at `4581edd`, ps5log `20260926T232940137Z`).
  - Pinball's frames reach the sink, but only two: at 31 s and 38 s.
  - Pinball had placed its window off-screen (see patch 0410), so the idle
    message loop of runs 12 and 13 was a window that never repainted.
- **Run 14** (patch 0410; `MenuHeight` removed from the console prefix;
  ps5log `20260927T061107832Z`).
  - From about 42 s the table is presented continuously: about 210 frames a
    second reach the sink, and the title shows 60 a second (`rejected=0`).
- **Run 15** (0410, `PW_WINE64_SCRIPT=1`, `PW_WINE64_SECONDS=75`; ps5log
  `20260927T061541751Z` to `20260927T061822316Z`).
  - launcher → Pinball → launcher → Pinball → launcher, then
    `wine64-script-done`.
  - Both Pinball runs present continuously. At the deadline the title posts
    Alt+F4, and Wine exits 1.25 s later (`reason=wine-exit`), inside the
    5 s wait before a forced close.

When a game exits, the launcher restarts the title with `LoadExec`. A test
script must therefore close the title until it stays closed before
restoring the eboot; otherwise the upload fails with `550 Text file busy`.

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
| `ntdll.prx` | 648,921 bytes | none | `libSceLibcInternal`, `libkernel`, `libkernel_sys` | `__stderrp`, `__stdoutp` (libc), `environ` (libkernel) |
| `win32u.prx` | 2,217,250 bytes | none | `ntdll.prx`, `libSceLibcInternal`, `libkernel` | none |
| `wineserver.prx` | 723,874 bytes | none | `libSceLibcInternal`, `libkernel`, `libkernel_sys` | `__stderrp`, `__stdoutp` (libc) |

- The data imports come from system modules the title already loads, not
  from another application PRX.
- The stubs have `isatty` only in `libScePosixForWebKit`, which a game title
  does not load. The compat layer provides it (no descriptor is a terminal),
  so ntdll needs only the libc and libkernel modules.
## Console result

The gate build (`PW_NATIVE_MODE=gate`) runs `wine/ps5/pw_wine_unix_probe.c`
when `PW_WINE_PS5_PRX_DIR` has packaged the two modules under `win/wine`.
It logs each step as `PW_WINE_UNIX` before running it.

On FW 12.02 (2026-09-26, log `20260926T105641663Z`), every step passed:

| Step | Result |
| --- | --- |
| Load `ntdll.prx` with `sceKernelLoadStartModule` | handle `0xd0`, start result 0, about 3 ms |
| Descriptor and exports | PRXDESC1 found in the module's segments; nothing missing |
| `module_start` and `pw_wine_dl_adopt` | 0; adopted |
| Heap | 64 B and 256 KiB allocated, written and freed: allocations 0→2, frees 0→2, one 1 MiB span mapped, no failures |
| `dlopen` of `win32u.so` through ntdll | `win32u.prx` loaded after ntdll, about 5.5 ms |
| `dlsym(win32u, "__wine_unix_lib_init")` | resolved |

The PRXs are staged under `win/wine` rather than `sce_module`. On the
console, `access()` reported the uploaded PRX absent in both places, while
`open()` succeeds, so the probe's presence check opens the file.
Wine-level initialisation (`__wine_main`, the wineserver) is not part of
this probe.
