# Hardware validation

This document records the public acceptance boundary. Raw telemetry, Remote
Play captures, proprietary input files and operator notes are not repository
inputs.

## Environment

- Owned PlayStation 5 running firmware 12.02.
- Native title identity: `PPSA99995` / `prospero-win`.
- First compatibility target: an original, privately supplied Space Cadet
  Pinball PE32 installation.
- Structured evidence: `ps5log/1` over TCP, correlated with exact artifact
  hashes and optional private Remote Play observation.

## Earlier results

Before the title ran Wine, a direct Win32 runtime (our own PE loader and a
reviewed subset of Win32, GDI, WinMM and user32) played Space Cadet Pinball
on this console with video, PCM audio, DualSense input, persistent registry
state and close/relaunch, and validated the DBT's direct chaining, register
residency and lazy flags on hardware. That runtime has been removed; the DBT
it validated is the one Wine's WoW64 layer now uses.

## Title self-restart with arguments

The planned launcher runs one game per title process: choosing a game
restarts the title with the game's profile as arguments, and closing it
restarts the title into the launcher, so every game starts Wine in a clean
process. A probe build (since removed) measured it.

On firmware 12.02 (2026-09-27, ps5log `20260927T000252036Z`,
`20260927T000255468Z` and `20260927T000258899Z`, baseline eboot restored):

- `sceSystemServiceLoadExec("/app0/eboot.bin", argv)` restarted the title
  twice in a row;
- each generation was a new process (pids 3030, 3031, 3032) and received the
  arguments intact, including `path=C:\Games\Pinball\PINBALL.EXE`;
- `argv[0]` is the first argument passed, not a program name; a launch from
  the system gives `argc=1` with an empty `argv[0]`;
- the restart took 431 and 430 ms, measured with `CLOCK_REALTIME` stamps
  passed as an argument.

`sceSystemServiceLaunchApp` on the title's own ID was not needed.

## Wine launcher: opening and closing Pinball

The wine64 title boots into the launcher and restarts itself for each game
(`docs/WINE_PS5_BUILD.md`, "Starting Wine in the title"). A scripted run
(`PW_WINE64_SCRIPT=1`, `PW_WINE64_SECONDS=75`) on firmware 12.02
(2026-09-27, ps5log `20260927T003053851Z` to `20260927T003344255Z`) staged
the Wine runtime beside the title under `win/wine`, used the
persistent prefix at `/data/prospero-win/prefix`, and deleted the runtime
and restored the baseline eboot afterwards:

1. The launcher started with no arguments (VideoOut and pad `ok`), chose
   Pinball and restarted the title with `profile=pinball`.
2. The game process loaded Wine (`stage=6`). Wine presented two 800x600
   frames of Pinball's window about 35 s in, and the title showed both.
3. At the 75 s deadline the title sent Alt+F4. Pinball did not close within
   5 s, so the title restarted into the launcher (`close-timeout`,
   `cycle=1`).
4. The same open and close happened a second time in a fresh process, and
   the launcher ended the script at `cycle=2`.

Two gaps remain:

- **Graceful close.** Alt+F4 did not close Pinball, so the return to the
  launcher took the forced path, which discards Wine's state without an
  orderly shutdown.
- **No redraw.** Wine presents the window only when it is painted. After the
  first two frames nothing redraws, so the screen shows a still picture of
  the table.

## DBT performance

On 2026-09-28 (FW 12.02) the title ran 7-Zip's benchmark, nbench's x87 build
and a Super PI-style pi program through the DBT, each as a launcher profile
in its own title process, with the stable baseline restored afterwards:

- 7-Zip rated 3361–3369 total MIPS in three rounds, against 1383 on
  2026-09-27 before the DBT's direct links, copied operands, call stack,
  superblocks and native FP;
- nbench completed all ten tests: integer index 166.7, FP index 81.6;
- pi computed 4.2M digits in 13 s and wrote its output file;
- with the working-directory fix (#201), each program started in its
  profile's working directory (`PW_WINE64 cwd=` with `status=0`);
- the null test passed before each series;
- x64 builds of the same programs ran natively: 7-Zip 5148–5152 MIPS,
  nbench 185.5 / 114.9, pi 8 s. Corrected for the x64 builds' own
  advantage (measured on the host), the DBT runs at about 85–93% of the
  console's native speed.

The ps5log runs, the host comparison and the method are in
[DBT benchmark](DBT_BENCHMARK.md#on-the-console).

## DXVK on the console's Vulkan

On 2026-09-28 and 2026-09-29 (FW 12.02) DXVK 2.6.2's test programs ran in
the title through Wine, one launcher profile per run, with the stable
baseline restored after each:

- on `ps5-vulkan`: the D3D8–D3D11 pixel controls, x64 and x86, read back
  both frames correctly, and a 32-bit process mapped host-visible memory
  below 4 GiB;
- on RADV (Mesa): the same pixel controls, the D3D8–D3D11 draw controls
  (shaders compiled on the console, a triangle and its background read
  back), x64 and x86, and 32-bit placed maps;
- on RADV, 1920x1080 D3D11 and D3D9 patterns filled the whole 4K screen,
  observed on the TV and in a Remote Play capture, as did a D3D11
  swapchain resized from 1080p to 4K in one process.

No Direct3D game has been run yet. Details and the fixes this took are in
[WINE_PS5_BUILD.md](WINE_PS5_BUILD.md#vulkan).

## Evidence rules

A screenshot or video proves appearance only. Runtime acceptance requires:

1. exact linked-ELF and fSELF identity;
2. exact package deployment and a fresh title mount after content changes;
3. a gap-free `ps5log/1` stream with the expected title and application;
4. frames shown on VideoOut and, when the game plays sound, an open audio port;
5. no classified abort, signal or ownership error;
6. either an orderly in-process `BYE` or an explicitly classified external
   system close.

The title's records and acceptance rules are in [TELEMETRY.md](TELEMETRY.md).
Private application files and raw evidence must remain outside the repository.
