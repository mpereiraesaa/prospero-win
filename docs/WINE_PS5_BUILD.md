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
sysctl), and `__PROSPERO__` selects the PS5 patches. PE modules come from
the host build that `tools/build_wine_runtime.sh` produces, which also
supplies winebuild and widl. The exception is the few PE modules a patch
changes (`PE_MODULES`, today the xinput DLLs), which are built from the
patched tree for i386 and x86_64 into `.deps/wine-ps5/pe`. That needs both
MinGW cross compilers.

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
| 0120 | `server`: when the current user names no audio driver, default `HKCU\Software\Wine\Drivers\Audio` to `ps5`; see [Audio](#audio) |
| 0130 | `ntdll`: a main program mapped away from its preferred base (the title's image may hold 0x400000) runs in-process, relocated by the PE loader; any other load failure prints the program and the status instead of running `start.exe`, which would need a new process. Both failure paths leave through `exit()`, so the title restarts into its launcher |
| 0140 | `ntdll`: `NtQueryDirectoryFile` enters a directory by the name the server opened it with when `fchdir()` on the server's descriptor fails. The descriptor has no path in ntdll's working-directory emulation, so no directory could be listed |
| 0150 | `server`: save the registry in 1 MiB writes: each `write()` costs about 3.3 ms on the console whatever its size, and stdio issued one per 64 KiB, so a 3.2 MB `system.reg` took 154 ms, during which the in-process server answers nothing |
| 0400 | `win32u`: in-process PS5 user driver (`WINE_PS5_USER_DRIVER`, set on PS5); see [User driver](#user-driver) |
| 0450 | `winevulkan`: `VK_KHR_display` becomes a host-only extension (in the Unix side's lists, never offered to applications, its client functions stay stubs and no thunk changes), so a driver can present through it; see [Vulkan](#vulkan) |
| 0455 | `win32u`: ask the host for `VK_KHR_external_semaphore_capabilities`, `VK_KHR_external_memory_capabilities` (WoW64) and `VK_KHR_external_fence_capabilities` (the D3DKMT instance) only when it has them; a Vulkan 1.0 host without them refused every instance |
| 0456 | `win32u`: a swapchain whose size differs from the window's gets `VkSwapchainPresentScalingCreateInfoEXT` chained in front of the application's structures instead of replacing them; dropping DXVK's format list while keeping its mutable-format flag crashed Mesa's WSI |
| 0457 | `win32u`: the WoW64 placed-map check reads its properties through `vkGetPhysicalDeviceProperties2KHR`; the core entry point is NULL on win32u's Vulkan 1.0 instances, so a host with `VK_EXT_map_memory_placed` (RADV) crashed every 32-bit process |
| 0460 | `win32u`: the PS5 user driver's Vulkan driver: a win32 surface becomes a host display-plane surface in the display's native mode, after the title releases its video output (before the first display query); installing the driver from Vulkan's initialisation does not refresh the display cache, which would wait on that initialisation |
| 0470 | `xinput`: controller 0 is the PS5 title's, read through a Unix library (`xinput1_3.so`) from the title's sink; elsewhere xinput uses HID as before; see [XInput controller](#xinput-controller) |
| 0500 | `ntdll`: signal context at `ucontext`+64 (measured); GS = TEB through `sysarch`; FS stays the libc TLS base, so the syscall dispatcher never switches it; no LDT for WoW64 threads |
| 0510 | `ntdll`: 16 KiB host pages under 4 KiB Windows pages, reusing the large-host-page path of `virtual.c` |
| 0520 | `ntdll`: name the ntdll directory with `WINE_PS5_NTDLL_DIR` when `dladdr` cannot (PRX) |
| 0530 | `ntdll`: with host pages larger than 4 KiB, store the x64 syscall-dispatcher pointer (0x7ffe1000) through the USD host page instead of mapping a separate page (needs the USD section sized to a host page) |
| 0540 | `ntdll`: count host `mmap`/`munmap`/`mprotect`, resolved faults and per-image cost (`__wine_virtual_stats`, `WINEDEBUG=+module`); skip an `mprotect` that leaves every host page of its range unchanged (tracked host protection). Pinball start on the host: 1,810 → 749 `mprotect` with 16 KiB pages, → 730 with 4 KiB |
| 0545 | `ntdll`: a fixed 16-entry table of the pages that fault most often (space-saving), with the fault kind, the page and host-page protection, how the fault ended and the last PC, read by the title through `__wine_virtual_fault_top` for its `fault_top` records |
| 0550 | `ntdll`: `NtCreateUserProcess` and `__wine_unix_spawnvp` return `STATUS_NOT_SUPPORTED` on PS5: a title can neither fork nor exec (the prefix is initialised offline and the desktop is created in process) |
| 0560 | `ntdll`: all Unix-side stderr goes through a sink the title sets with `__wine_ps5_set_output_sink` (a title cannot give Wine a usable fd 2); `fatal_error` formats into a local buffer |
| 0570 | `ntdll`: reserve address space with a fixed, no-overwrite `sceKernelReserveVirtualRange`, since `MAP_FIXED \| MAP_EXCL` replaces existing mappings on FW 12.02; the view heap gets 64 MiB above 4 GiB |
| 0580 | `ntdll`: skip the configuration directory's parent ownership check on PS5, as 0111 skips the one after `chdir` |
| 0600 | `ntdll`: anonymous memory in the reserved areas is direct memory, not flexible memory; a 16 GiB area at `0x1000000000` takes the allocations free to go anywhere; see [Direct memory](#direct-memory) |
| 0601 | `ntdll`: an i386 image is never mapped above 4 GiB, so a relocatable exe whose preferred base is taken stays in the low reserved areas instead of the high one |
| 0610 | `ntdll`: `__wine_ps5_set_segv_hook` lets `wowprospero` resume its own faults (fault markers) from inside Wine's SIGSEGV handler, instead of a second handler chaining to the action `sigaction` reports |

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

## Direct memory

A title has about 440 MiB of flexible memory, which every anonymous mapping
draws from, and up to 12 GiB of direct memory, which it allocates by physical
offset and maps where it chooses (FW 12.02). Mappings without an address
share one region at `0x200000000` that fills near 384 MiB. Patch 0600 sends
the host `mmap`, `munmap` and `mprotect` of `virtual.c` (the counting
wrappers of patch 0540) through `wine/ps5/pw_wine_dmem_ps5.c`, and every
reserved area becomes a region of `wine/ps5/pw_wine_dmem.c`:

- a fixed anonymous mapping (a view, a commit through `anon_mmap_fixed`) is
  direct memory, zeroed, mapped read-write and then protected, since the
  kernel refuses execute permission at map time but grants it through
  `sceKernelMprotect`;
- `PROT_NONE` (decommit, release) leaves a reservation and releases the
  direct memory at once;
- an `mprotect` that makes reserved pages accessible commits them;
- a private file view (an image section) is refused with `ENODEV`, so
  ntdll reads the file into the direct memory already there, as it does on a
  file system without `mmap`: on the console a fixed file mapping over a
  reservation left the pages unusable and the loader faulted writing a
  section's tail; a shared one (shared memory) is mapped where asked, over
  what is there, and takes the place of the direct memory;
- `munmap` releases the memory and the address space; the range stays
  owned, so a later fixed mapping there is direct memory again.

The allocation recipe is the one the ps5-xash3d heap runs on (main direct
memory of type `0x0c`, a fixed map over a reservation), mapped CPU read-write
unless the kernel wants the GPU bits xash3d maps with. Before the first
region is used, a self-check maps a page, writes it, makes it read-execute
and frees it; if the console refuses any step, every call passes through as
before and the log says so. A 16 GiB reserved area at `0x1000000000`, where
the kernel grants the whole range at the hint, takes the views whose limits
allow it before the low areas are searched, so the i386 guest keeps the low
4 GiB. An i386 image's limit is 4 GiB (0601): the main exe is mapped where
it can when its preferred base is taken, and before 0601 that was the high
area, where the guest saw its base truncated to 32 bits. The host tests run the policy against a model of the kernel (a memfd
for physical memory, reservations that refuse to overlap, execute refused at
map time) with 20,000 random operations checked page by page.

## PRX modules

The console does not load ELF shared objects, so after the ELF link the
script links `ntdll` and `win32u` again as PRX modules into
`.deps/wine-ps5/prx/sce_module/`:

1. Each module takes the objects of its ELF link, read back from
   `make.log`. `ntdll` adds the heap, direct memory and the PS5 shims (`pw_wine_prx`,
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
- **Cursor.** No display server draws the pointer, so the driver blends the
  application's cursor into a copy of each frame (patch 0420). The pointer
  reaches every pixel. At the right and bottom edges, an arrow's top-left
  hotspot used to leave only its one-pixel tip inside the frame. Patch 0440
  moves the drawn cursor just enough to keep its visible pixels inside the
  frame at every edge; the pointer itself does not move.
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
- Cursor edges (2026-09-27, patches through 0440). The fixture moved
  winemine's pointer to each edge (`PW_INPUT_MOVES`, `PW_FRAME_DIR`), then to
  the bottom-right corner (799,599). Every frame showed the whole arrow; at
  the corner its white pixels covered (785,576)-(797,597). Without 0440,
  the frames at x=799 and y=599 showed only a one-pixel column or row of the
  arrow.
- No text was drawn in that host build (`--without-freetype`). The PS5 build
  now has fonts: see "Fonts" below.

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

## Fonts

`tools/build_wine_ps5.sh` builds FreeType from a pinned release (2.13.3,
SHA-256 checked; `PROSPERO_FREETYPE_TARBALL` names a local copy) with the
payload SDK. The build keeps only the TrueType, CFF, Type 1 and Windows `.fon`
drivers, the hinters, the two rasterisers and FreeType's own gzip.

- Wine is configured with FreeType found through `FREETYPE_CFLAGS` and
  `FREETYPE_LIBS`. Its soname is `libfreetype.so`, the name
  `dlls/win32u/freetype.c` passes to `dlopen`.
- The PRX stage links `libfreetype.prx`. Its descriptor exports the 31
  functions win32u loads with `dlsym`, so Wine's own `dlopen` loads it
  through `pw_wine_dl`. It is staged beside `ntdll.prx`. The bare soname
  has no directory, so `pw_wine_dl` looks for it in its module directory,
  then beside each module already loaded (ntdll.prx). It never tries a bare
  name as given: the console refuses relative paths.
- A `dlopen` that fails writes one line to stderr, and so to ps5log:
  `pw_wine_dl: cannot open <name>, tried <paths>: <reason>`.
- Wine's 13 fonts (`fonts/*.ttf`: Tahoma, MS Sans Serif, Courier, System,
  Marlett and others) are copied to `<work>/prx/fonts`.
- **Stage them in the prefix's `drive_c/windows/Fonts` on `/data`.**
  win32u finds fonts by listing two directories: Wine's `share/wine/fonts`
  and `C:\windows\Fonts`. The first is inside the read-only application
  image, which a title cannot list (`opendir` and `getdents` fail there). On
  `/data`, libc `readdir` works once `/data` is granted (measured
  2026-09-27), and patch 0140 lets Wine list a directory whose descriptor
  came from the in-process server.
- Without them FreeType loads, but win32u logs `can't find a single
  appropriate font`: windows show no captions, menus or caption buttons.

## Audio

Wine 11's mmdevapi loads an audio driver as a bare Unix library by name:
driver `ps5` is `wineps5.so`, which `pw_wine_dl` loads as `wineps5.prx`
beside `ntdll.prx`. It takes the name from
`HKCU\Software\Wine\Drivers\Audio`. None of the drivers in its default
list (`pulse,alsa,oss,coreaudio`) exists on the console, so patch 0120 makes
the in-process server set the value to `ps5` when a prefix does not name
one. A prefix that names another driver keeps it, and an empty value still
means no audio.

`wine/wineps5/unix.c` is the driver's Unix side, built and linked by the PRX
stage like `wowprospero.prx`. Its stream, buffer and WoW64 code follows
Wine's OSS driver.
- It offers one render endpoint, `PS5`, and no capture.
- Its mix format is 48 kHz float stereo. A stream may use any PCM or float
  format `src/pw_audio_mix.h` decodes: winmm and dsound open theirs with
  `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`.
- One Unix thread mixes every started stream into a grain of 256 16-bit
  stereo frames, then passes it to `pw_wine_audio_output`. That is the audio
  sink ntdll exports (`wine/ps5/pw_wine_sink.h`); the driver finds it with
  `dlsym`.
- The title plays each grain on the console's main port with
  `sceAudioOutOutput`, which returns once the port has taken it. The port is
  therefore the clock, and each stream's event is set once per period.
- It reports itself unavailable when the title set no sink, so mmdevapi
  offers no device.

To use it on the console, stage `wineps5.prx` beside `ntdll.prx`. The game
log then shows `PW_WINE64 audio sink=1 port=<handle> status=ok`.

## XInput controller

Games played with a controller ask XInput for one. Wine's xinput normally
finds controllers through its HID stack (winebus and hidclass), which runs
in `winedevice.exe`. A title cannot start that process (patch 0550), so on
the console xinput never found a controller. Patch 0470 lets xinput read
the title's DualSense instead:

- The title keeps the pad's state in its sink, in `XINPUT_GAMEPAD` terms.
  It calls `pw_wine_set_pad` in `wine/ps5/pw_wine_sink.h`, and the sink
  numbers a packet each time the state changes.
- xinput gains a Unix library, `xinput1_3.so`. Every xinput DLL built from
  `xinput1_3`'s source loads it by name: 1.1, 1.2, 1.3, 1.4 and uap;
  9.1.0 forwards to 1.4.
  - Its init finds `pw_wine_pad` and `pw_wine_set_rumble` with `dlsym`.
    Anywhere but in a title they are missing, and xinput keeps using HID.
  - With a title, controller 0 is the title's and no HID thread starts.
    `XInputGetState(Ex)` and `XInputGetKeystroke` read the sink, and
    `XInputSetState` and `XInputEnable` pass the rumble back.
    `XInputGetCapabilities(Ex)` reports a wired Xbox 360 controller
    (`045e:028e`), which games look for.
- The parameters hold no pointers, so a 32-bit game's calls go through the
  same functions under WoW64.
- The PRX stage links `xinput1_3.prx`. Its descriptor exports
  `__wine_unix_call_funcs` and `__wine_unix_call_wow64_funcs`, and it
  imports ntdll's `dlsym`, like `wowprospero.prx`.
- The patched PE modules (`xinput1_1`, `xinput1_2`, `xinput1_3`,
  `xinput1_4` and `xinputuap`) are built for i386 and x86_64 into
  `.deps/wine-ps5/pe/<arch>-windows`. `report.json` lists them under `pe`
  with their hashes.

To use it on the console:
- stage `xinput1_3.prx` beside `ntdll.prx`;
- replace the runtime's xinput DLLs in `lib/wine/i386-windows` and
  `lib/wine/x86_64-windows` with those from `pe/`, and also the copies in
  the prefix's `syswow64` (i386) and `system32` (x86_64);
- give the game's profile `[input] mode = xinput` (see
  [Starting Wine in the title](#starting-wine-in-the-title)).

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

## Vulkan

Direct3D games use DXVK, which runs on Vulkan, which on the console is
either [ps5vk](https://github.com/mpereiraesaa/ps5-vulkan) or RADV, Mesa's
AMD driver ([mpereiraesaa/PS5_Mesa](https://github.com/mpereiraesaa/PS5_Mesa),
built by [mpereiraesaa/PS5_Vulkan](https://github.com/mpereiraesaa/PS5_Vulkan)).
Both are a `libvulkan.prx`; nothing else in the runtime changes. The chain:

```text
game (PE) -> DXVK d3d11/dxgi/d3d9/d3d8/d3d10core (PE, beside the game) -> winevulkan.dll
  -> winevulkan.prx (Wine's Vulkan Unix side) -> win32u.prx (PS5 driver, patch 0460)
  -> libvulkan.prx (ps5vk or RADV) -> AGC and VideoOut
```

- **Build.** Configure no longer disables Vulkan, and names the library
  `libvulkan.so`, which `pw_wine_dl` loads as `libvulkan.prx` beside
  ntdll. `--ps5vk-sdk DIR` (a ps5vk `dist-sdk`) links `libvulkan.prx` from
  `libps5vk.a` and `libpsbc.a`: only what `vkGetInstanceProcAddr` and
  `vkGetDeviceProcAddr` reach, with ps5vk's AGC import facades beside the
  SDK's stubs. `wine/ps5/pw_vulkan_libc.c` supplies the few libc functions
  it names that a title lacks (`popen`, `pclose`, `mkstemp`, `__assert`).
  Without the SDK the module is skipped. `winevulkan.prx` and
  `opengl32.prx` are built either way. The console has no OpenGL, but
  `wined3d`, which Wine's `d3d10.dll` imports even over DXVK, needs
  `opengl32` to initialise, and it does so with no driver. ps5vk is
  GPL-3.0-or-later, so a title that ships ps5vk's `libvulkan.prx` ships a
  GPL work.
- **RADV.** `tools/build-radv.sh release` in PS5_Vulkan builds RADV's
  archive (`libvulkan_radeon.ps5.a`) from its pinned PS5_Mesa revision. It
  exports only Vulkan's ICD entry points, so the `libvulkan.prx` Wine loads
  adds two functions (`wine/ps5/pw_vulkan_radv.c`): `vkGetInstanceProcAddr`
  calling `vk_icdGetInstanceProcAddr` and `vkGetDeviceProcAddr` calling
  `vk_common_GetDeviceProcAddr`. `build_wine_ps5.sh --radv DIR`, with DIR
  that PS5_Vulkan checkout, links it in place of ps5vk's
  (`tools/link_radv_prx.sh`, with PS5_Vulkan's own `tools/radv-link.sh`
  recipe and payload SDK), and reports the PS5_Mesa revision; `--radv` and
  `--ps5vk-sdk` are exclusive. Mesa is MIT-licensed.
- **Presentation.** Applications see `VK_KHR_surface` and
  `VK_KHR_win32_surface`. The PS5 driver creates the host surface with
  `vkCreateDisplayPlaneSurfaceKHR` on the driver's one display and plane,
  always in the display's native mode, the one at its physical resolution
  (3840x2160 with RADV on a 4K TV, 1920x1080 with ps5vk); the desktop is
  scaled to it, never the reverse (patch 0460). Before its first display
  query the driver asks the title to release its video output
  (`pw_wine_release_display`): RADV opens VideoOut as soon as its displays
  are enumerated, and caches a failed open for the whole process, while
  ps5vk opens it with the swapchain.
- **Swapchain size.** The swapchain is the window's size, normally the
  profile's desktop. RADV takes any size up to the mode's and VideoOut
  scales it to the whole screen (PS5_Mesa #1; 1920x1080 on 3840x2160 was
  measured). ps5vk takes only 1920x1080, so with ps5vk a game's desktop
  should be 1920x1080. When Wine has to create a host swapchain larger than
  the window it adds `VkSwapchainPresentScalingCreateInfoEXT` in front of
  the application's structures (patch 0456); dropping them crashed DXVK's
  swapchain in Mesa's WSI.
- **DLLs.** DXVK's DLLs go beside the game, and its profile sets
  `dll_overrides` (`d3d11,dxgi=n`, `d3d9=n`, `d3d8,d3d9=n` or
  `d3d10core,d3d11,dxgi=n`). The unmodified Win32-WSI DXVK build is the one
  to use, since the PS5 surface is Wine's.
- **32-bit games.** Wine's WoW64 thunks carry Vulkan calls from the DBT.
  Host-visible memory must be mapped below 4 GiB for 32-bit code: ps5vk
  returns such addresses itself through a low CPU alias, so Wine's plain
  `vkMapMemory` path works without `VK_EXT_map_memory_placed` or
  `VK_EXT_external_memory_host`. RADV offers `VK_EXT_map_memory_placed`,
  which Wine uses (alignment 16384); patch 0457 lets it query that
  extension's properties at all.

Console results (FW 12.02, 2026-09-28 and 2026-09-29). The test programs
are ps5vk's DXVK 2.6.2 PE frontends (one per API), with the unmodified
Win32-WSI DXVK DLLs: each creates a 1920x1080 window, clears two frames to
known colours and presents each. Their `-pixels` variants also read the
back buffer back (`GetRenderTargetData`/`LockRect`, or a staging copy and
`Map`) and check the centre pixel before each `Present`.

| Program | Result |
| --- | --- |
| Vulkan probe, x64 and x86 | Instance, physical device, win32 surface, device and swapchain all succeed, and teardown is clean |
| DXVK 2.6.2 D3D11, D3D10, D3D9, D3D8 pixel controls, x64 and x86 | Both frames read back with the expected centre pixel (`844c1cff`, then `1c4c84ff`) and no mismatches; every call returns `S_OK`; ps5vk logs a completed flip for each frame (`PS5VK_VIDEO_PRESENTED`, tokens 1 and 2) and no refusal |
| ps5vk's `vkmap` probe, x86 | Two host-visible buffers map at 32-bit addresses (`0x818e0000`, `0x81900000`) through ps5vk's low CPU alias, and a 1,024-byte GPU copy between them has no mismatches |

All of these ran on one ps5vk SDK (`libps5vk.a` SHA-256 `98f8a17c…`),
whose changes are merged in ps5vk (through PR #607). Along the way ps5vk
gained DXVK's mutable BGRA8 back buffer, its barriers, clear-only render
passes, transfer-only submissions, the D3D9 presenter's `R,G,B,ONE` view and
the back buffer's hand-back after a readback. The pixels checked are the back
buffer's, read through the API; the physical scanout was not captured.

Console results with RADV (FW 12.02, 2026-09-29): Mesa's release archive
linked as `libvulkan.prx`, the same staged runtime otherwise, stable build
restored after each run. The draw controls also compile a vertex and a pixel
shader on the console (Wine's `d3dcompiler_47`), draw a triangle and read
back its centre and a corner of the background. The scanout controls hold a
four-colour 1920x1080 pattern for 30 frames.

| Program | Result |
| --- | --- |
| Vulkan probe, x64 | Instance, device `PlayStation 5 GPU (RADV NAVI21)`, win32 surface on the 3840x2160 59.94 Hz plane, device and swapchain all succeed |
| DXVK 2.6.2 D3D11, D3D10, D3D9, D3D8 pixel controls, x64 and x86 | 8/8: both frames read back `844c1cff`, then `1c4c84ff`, no mismatches, every call `S_OK` |
| DXVK 2.6.2 D3D11, D3D10, D3D9, D3D8 draw controls, x64 and x86 | 8/8: triangle centre `0000ffff` and both backgrounds in each frame, both presents `S_OK` |
| `vkmap` probe, x86 | Two host-visible buffers map at 32-bit addresses (`0xac0000`, `0xad0000`) through placed maps; a 1,024-byte GPU copy has no mismatches |
| D3D11 and D3D9 1920x1080 scanout controls | Pass, and the pattern fills the whole screen on the TV and in a Remote Play capture (before PS5_Mesa #1 it sat in the top-left quarter) |
| D3D11 3840x2160 control; one D3D11 swapchain resized from 1920x1080 to 3840x2160 | Pass; the resize shows correctly on the TV |

The pixel, draw and `vkmap` rows ran on PS5_Mesa `cedb774` before the
scaling change; D3D11 draw x64, D3D9 draw x86 and the probe ran again after
it. Swapchain sizes other than 1920x1080 and 3840x2160 are untested.

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
prospero-win (`native/pw_data_mount.c`) asks the Lapy JB daemon
(<https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon>, running on the
console) for `/data`, following its protocol: in the title's own process,
before it creates any other thread, `seteuid(geteuid())`, which must succeed
(otherwise nothing is requested and the log says why, `prepare_errno`); then
a complete `{"PID":<pid>}` request, written to a temporary file and renamed
to `/download0/elevate_proc` (the title's `param.json` enables
`downloadDataSize` for `/download0`). The daemon consuming the request proves
nothing, so the title waits until `/data` is actually reachable, after which
the prefix lives at `/data/prospero-win/prefix`. The earlier daemon's
`/download0/etahen_jailbreak` marker is no longer written. Once `/data`
appears after a request, the title waits another second
(`PW_DATA_MOUNT_SETTLE_MS`) before it goes on. The request is best-effort:
if the daemon is not running, the title keeps using
`/download0/prospero-win/prefix`.

## Starting Wine in the title

`tools/build_native.sh` builds the title around `native/wine64_main.c`.
The title runs one game per process (`src/pw_wine_launch.h`):

- **Launcher.** Started with no game (as the system starts it), the title
  shows the launcher (`src/pw_launcher_render.c`) on VideoOut without
  loading Wine. The d-pad moves the selection. Cross restarts the title with
  `sceSystemServiceLoadExec` and the game's arguments
  (`profile=pinball path=C:\Games\Pinball\PINBALL.EXE cycle=<n>`). A
  restart is a new process that receives its arguments intact in about
  430 ms (see `HARDWARE_VALIDATION.md`).
- **Library.** The games live in `/data/prospero-win`
  (`native/pw_wine_library.h`). Nothing is built in: a game appears by adding
  `profiles/<name>.profile`, in the format of `src/pw_game_profile.h`. The
  directory is read from `profiles/profiles.lst` when present, and listed
  with `getdents` otherwise. Refused profiles are listed as not available and
  logged with the reason.
- **`/data`.** The launcher and each game request `/data` when they start.
  A process granted `/data` cannot write the sandbox's `/download0`
  (`EACCES`, measured), so a game cannot leave the launcher a copy of the
  library there.
  [prospero-win-profiles](https://github.com/mpereiraesaa/prospero-win-profiles) has the same layout, with the profiles
  checked on the console and their shared input presets.
- **Game.** Started with `profile=` or `path=`, the title runs that executable
  in Wine as below, with the profile's settings:
  - `prefix = default` uses `<root>/prefix`, any other name
    `<root>/prefixes/<name>`;
  - `dll_overrides` (optional, in `[application]`) sets `WINEDLLOVERRIDES`
    for that game only. `d3d11,dxgi=n` makes it use the DXVK DLLs beside
    its executable instead of Wine's builtins, without touching the prefix's
    registry, which every game shares;
  - `[display] desktop` sets `WINE_PS5_DESKTOP`, and `scaling` (`fit`,
    `integer` or `stretch`) scales each frame onto the whole 1920x1080
    screen. `fit` keeps the aspect ratio, so 800x600 is shown at 1440x1080;
  - `[display] view = window` (the default) sets `WINE_PS5_VIEW=window`: Wine's
    driver (patch 0430) presents only the game's visible windows, so a small
    game fills the screen; `view = desktop` shows the whole Wine desktop;
  - `[input]` binds each DualSense button to a key or a mouse button, and a
    stick moves the pointer. `preset = <name>` shares a mapping from
    `<root>/input/<name>.input`.
  - `[debug] winedebug` sets `WINEDEBUG` for that game in place of the
    title's (`err+all,+loaddll,+process`), without rebuilding it, e.g.
    `winedebug = err+all,+seh` for one run. Only a channel list is taken
    (letters, digits and `_ + - , = .`), and the log names it
    (`PW_WINE64 winedebug=`). Change it and push the profile again.
  - `mode = xinput` also makes the DualSense the game's XInput controller
    0 (see [XInput controller](#xinput-controller)).
    - Cross, Circle, Square and Triangle are A, B, X and Y. L1/R1 are the
      shoulders, L2/R2 the analog triggers and L3/R3 the stick clicks.
      Options is Start, Create is Back and the touchpad is Guide.
    - The sticks are scaled to XInput's range, with y up.
    - The rumble a game asks for runs the DualSense's motors through
      `scePadSetVibration`: XInput's left motor is the large one. The
      motors stop when the game is asked to close. The first rumble, and
      any the pad refuses, are logged as `PW_WINE64 rumble`.
    - Bindings and the pointer stick still apply, so a preset can add a key.
      The `gamepad` preset in prospero-win-profiles binds nothing and moves
      no pointer.
    - The game log's `PW_WINE64 display` line shows `xinput=1` when the
      runtime's ntdll has the gamepad slot.
- **Closing.** Holding Options+Create for a second sends the game Alt+F4. When
  Wine exits, the title restarts into the launcher. If the game has not
  closed after 5 s, the title restarts into the launcher anyway.
- **Unattended validation.** `-DPW_WINE64_SCRIPT=1` makes the launcher open
  games by itself, one per cycle in the library's order (file names sort
  it), `PW_WINE64_SCRIPT_CYCLES` times (2 unless the build sets it; each
  cycle is two sandbox escapes, the game and the launcher).
  `-DPW_WINE64_SECONDS=<s>` closes each game after that long; the default, 0,
  lets a game run until it is closed.

In a game, the title works as follows:

1. It requests the `/data` mount (`native/pw_data_mount.c`) and uses
   `/data/prospero-win/prefix`, or
   `/download0/prospero-win/prefix` when `/data` does not appear.
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
d-pad nudges, Options pauses and Square starts a new game. Frames up to 1280x1024 are shown; larger ones are
counted as rejected.

A game that presents with Vulkan scans out through the GPU driver's own
video output, so the title hands its own over first. The driver calls
`pw_wine_release_display` when the game creates its first Vulkan surface
(patch 0460). That calls the title's callback, which waits until the main
thread has closed the title's VideoOut (logged as `PW_WINE64 display released
to vulkan`). From then on the sink passes no more GDI frames to the title,
and the sink statistics show `display_released`. A game runs in its own
process, so the release lasts until the game exits.

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
  `win/wine/lib/wine/x86_64-unix`, with `xinput1_3.prx` for games played in
  xinput mode, and `winevulkan.prx`, `opengl32.prx` and `libvulkan.prx` for
  Vulkan and Direct3D ([Vulkan](#vulkan)), and `ws2_32.prx`, Winsock's
  Unix side, which games import even offline (Warcraft III's `War3.exe`
  stops at start without it). A title has every socket call it makes but
  the old resolver's `gethostbyaddr` and its `h_errno`, which
  `wine/ps5/pw_ws2_32_libc.c` provides (a reverse lookup through
  `getnameinfo`, and a per-thread error); and `crypt32.prx`, CryptoAPI's
  Unix side, without which `crypt32.dll` refuses to load: FFmpeg's
  `avformat` imports it, so LAV Filters, the DirectShow splitter and
  decoders Warcraft III's cinematics play through, need it (the console's
  Wine has no GStreamer, which Wine's own splitters are built on);
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

- **Run 16** (main at `eb548ef`, played by hand with a DualSense; ps5log
  `20260927T063212997Z` to `20260927T063351187Z`).
  - Cross in the launcher chose Pinball.
  - The player started a game (Square), launched the ball (Cross) and used
    the flippers (L1/R1). All 25 pad events reached Wine (`inputs=25
    refused=0`), and the game responded on screen.
  - 2,444 frames were shown at 60 Hz, with `rejected=0`.
  - Holding Options+Create requested the close (`by=combo`). Wine exited
    1.3 s later, and the title returned to the launcher.

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

A probe build (since removed) loaded the two modules packaged under
`win/wine` and logged each step as `PW_WINE_UNIX` before running it.

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
