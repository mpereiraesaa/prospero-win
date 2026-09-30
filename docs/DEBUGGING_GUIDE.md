# Debugging guide

How we find out why a game doesn't start, draws nothing, or misbehaves on
the PS5. It is the method behind the Warcraft III fixes (startup, cinematics,
display modes, input), written down so the next game goes faster.

This is a living document. When you find a new trick, a faster loop or a
symptom with a surprising cause, add it: a line in
[symptoms we've seen](#symptoms-weve-seen) is often enough.

## The loop

1. Reproduce on the PC if you can.
2. Read the log, not the screen.
3. Turn on the Wine channels for the part you suspect.
4. Check the build before you touch the console.
5. Keep console runs short, unattended and self-restoring.
6. Change one thing at a time.
7. Write down what was measured.

## 1. Reproduce on the PC first

A console cycle takes minutes; a run on the PC takes seconds. Start there.

```sh
systemd-run --user --scope -q -p MemoryMax=6G \
    tools/run_wine_dbt_host.sh --profile <game>.profile --stage <game dir> \
    --cpu dbt --screenshot shot.png --log run.log
```

`run_wine_dbt_host.sh` runs the game's 32-bit exe under the pinned host Wine
(`tools/build_host_wine.sh`, the same revision and patches as the console)
with our translator as Wine's 32-bit CPU. `--cpu native` runs it with Wine's
own `wow64cpu` instead: if the game fails with both, the problem is in Wine or
the prefix, not the translator; if only `--cpu dbt` fails, it's the
translator. Use the pinned Wine, not the distribution's: a different version
fails in its own ways (missing services, prefix setup errors) that say
nothing about the PS5.

Cap the memory of every host run (`systemd-run ... -p MemoryMax=`) and run
one at a time; an emulated game can take the whole machine with it.

Some problems only exist on the console: the display driver, VideoOut,
direct memory, the PS5's input devices. The PC still tells you whether the
game itself gets that far.

## 2. Read the log, not the screen

The app sends a `ps5log/1` log over TCP to the PC named in `dev.conf`
([getting a log](GETTING_STARTED.md), [every record](TELEMETRY.md)). Wine's
own output arrives in the same stream, one line per record. Read the records
in the order the app writes them:

| Record | The question it answers |
| --- | --- |
| `PW_WINE64 args ... mode=game profile=<id>` | Did the right profile start? |
| `profile`, `winedebug` | Were its desktop, view, input and debug channels read as you meant? |
| `ntdll`, `load`, `environment`, `run` | Did Wine's runtime load and start? |
| `display` | Are the present sink, input and XInput hooks and VideoOut there? |
| `alive` (about once a second) | Is it drawing (`frames_put`, `vk_shown`) and taking input (`inputs`, `refused`)? |
| `fault_top` | Which pages fault most, from which PC? |
| `exit`, `restart to=launcher` | How and when did it end? |

Useful readings:

- `alive` keeps coming but `sink_calls` and `vk_shown` stay at 0: the game is
  stuck before its first frame. Look at Wine's lines just before it.
- `inputs` grows and `refused` stays 0, but the game doesn't react: the input
  reaches Wine and is lost inside it (the display driver, focus, coalescing).
- One page in `fault_top` with tens of thousands of faults a second is a
  loop, not a workload.
- `cpu_ms` tells a wait from a spin: during a stall, a spinning thread adds
  about 1000 a second, a waiting process almost nothing.

When a stall disappears as soon as you turn on Wine's channels (they make
every thread slower and talk to the server more), build the app with
`PW_WINE64_WAIT_WATCHDOG=1` instead. Every two seconds Wine's server then
logs what each thread waits on, each message queue's state and every
pending async I/O, a few lines at a time, so the timing barely changes. A
thread the game needs that is in no server wait at all is blocked in
host-side code.

## 3. Turn on Wine's channels for one game

Set `WINEDEBUG` in the game's profile, not for the whole app:

```ini
[debug]
winedebug = err+all,+loaddll,+process,+seh
```

| Suspect | Channels |
| --- | --- |
| A DLL or import is missing | `+loaddll`, `+module` |
| A crash or an exception | `+seh`, with `+virtual` for page faults |
| Window, display mode or cursor | `+win`, `+cursor`, `+system` |
| Input | `+keyboard`, `+cursor`, `+rawinput` |
| OpenGL | `+wgl`, `+opengl` |
| Movies (DirectShow) | `+quartz`, `+strmbase` |

Channels are slow and the log is sent over the network: turn on what you
need, then turn it off again. The app also names Wine's startup steps
(`WINE_PS5_TRACE_STARTUP`, patch 0560), so a hang during startup shows the
step it stopped at.

## 4. Check the build before the console

Every console module has to link against what the PS5 provides. After
`tools/build_wine_ps5.sh`, its report (`.deps/wine-ps5/report.json`, and one
line per module in the build output) must show `built=True unresolved=none`
for every PRX, and its `data_imports` must be ones the PS5's loader
provides. A missing symbol is cheap to find here and expensive on the
console, where the module just fails to load.

Run `make all` too: the host tests, the sanitizers in CI and the publication
audit catch most mistakes in the app itself. They don't run the app's
`main()`, so read changes there twice.

## 5. Short, unattended, self-restoring console runs

- **Upload only what changed.** The app folder is about 570 MB; a Wine fix
  usually changes one or two modules. Compare against the build that is
  installed and upload the difference.
- **Let the launcher drive.** Build the app with `PW_WINE64_SCRIPT=1`
  (`tools/build_native.sh`): the launcher opens the games in the library's
  order by itself. `PW_WINE64_SECONDS=<n>` closes each game after n seconds,
  `PW_WINE64_SCRIPT_CYCLES=<n>` sets how many it opens. Stop as soon as the
  log says the game finished.
- **Always restore,** even when the run fails or is interrupted: put back the
  files you replaced and any profile you added, and check them.
- **Compare hashes with the FTP server's decryption off.** Some PS5 FTP
  servers decrypt signed files (`eboot.bin`, PRX modules) as you download
  them, so the hash you read back differs from the file you uploaded.
- **Someone may be using the console.** If an app is running, wait; don't
  close it.

## 6. Change one thing at a time

Start from the smallest program that shows the problem: a tiny test exe is
better than the whole game. Then change one thing per run and compare the
logs:

- the profile: `graphics`, `dll_overrides`, `[display]` size, `view`
  (`window` or `desktop`), input mode;
- Wine's DLLs: builtin or native (`dll_overrides = d3d9=n` and so on);
- the prefix: a fresh one from the recipe, against the one you've been
  changing by hand.

Screenshots (from Remote Play or the TV) are for the last check, when the
log already says it works.

## 7. Write down what was measured

A result is worth something when someone else can repeat it. Record:

- the firmware, the app's `eboot.bin` hash and the hashes of the modules you
  changed;
- the log file of the run;
- whether each claim was measured on the console or only assumed (on the PC,
  or from documentation).

"The PS5 can't do X" is a claim to measure before you write it down.

## Symptoms we've seen

What each looked like, and what it turned out to be. Newest first.

| Symptom | Cause | Fix |
| --- | --- | --- |
| Warcraft III's intro started 0.4–32 s after its decoder was ready (twice never); the app idle at 0.1 cores meanwhile; heavy logging or a key press made it go away | Lock starvation in Wine's DirectShow ([#250](https://github.com/mpereiraesaa/prospero-win/issues/250)). `IBaseFilter::GetState()` held the renderer's filter lock while waiting for it to finish pausing, and the graph polls it every 10 ms during `Run()`; the thread delivering the first frame needed that lock and, woken ~1 ms after each release on the PS5, always found it taken again. Found with the wait watchdog (`PW_WINE64_WAIT_WATCHDOG=1`), which named the waiting thread's critical section and its owner | Wine patch 0700: the renderers wait with the filter lock released. The intro now starts 0.19 s after the decoder is ready, every time. The LAV settings in the recipe were a red herring |
| The stick and USB mouse didn't move the pointer; keys worked. `inputs` grew, nothing refused | win32u (Wine 11) holds a driver's mouse motion until a button or `MOUSEEVENTF_MOVE_NOCOALESCE` sends it; the PS5 driver never sent it. Before, the pointer only seemed to move because the driver answered `GetCursorPos` itself | Wine patch 0670: relative motion, sent with `MOUSEEVENTF_MOVE_NOCOALESCE` |
| An RTS map didn't scroll at the screen's edges | The app sent absolute positions, and nothing while the pointer was held at an edge; the game's own `SetCursorPos` was overridden | Patch 0670: the app sends motion, Wine keeps the only pointer |
| A white rectangle while the game loaded | win32u fills a new window surface with white; a Direct3D window is never painted | Patch 0650: new surfaces start black on the PS5 |
| 800x600 cinematics in the top-left corner | The PS5 driver refused display mode changes | Patch 0630: accept modes up to the desktop's size and show the part they cover |
| The screen went black at 1440x1080 | VideoOut refuses that size | Use 1920x1080 (for Warcraft III, with RenderEdge for 16:9) |
| GDI frames shown unscaled after the Vulkan handover | The frames' pixel format; VideoOut only scales B8G8R8A8 | Draw GDI frames as B8G8R8A8 |
| Movies didn't play | No GStreamer on the PS5; also crypt32's Unix side was missing | LAV Filters in the prefix (winetricks `lavfilters`); build `crypt32.prx` |
| A game crashed on instructions with the `bnd` prefix, or took AVX paths | The translator rejected `bnd` branches; CPUID reported AVX and friends | Accept `bnd` (it's a no-op); hide AVX, AVX2, FMA, F16C, BMI, AVX-512, XSAVE and XOP from CPUID |
| The USB keyboard and mouse modules failed to load | Loaded by name or path (`ENOENT`, `ESDKVERSION`) | Load them as system modules (`sceSysmodule`) and look their functions up |
| Cinematics stuttered | The driver's flush period, counted regardless of the last forced flush (a shorter period alone didn't help) | Patch 0640: 16 ms counted from the last forced flush |
| About 3 GB used by Warcraft III | The translator committed its large code arenas up front | Reserve them and commit 1 MiB at a time as they fill (now about 600–650 MB in the menus) |

## Adding to this guide

Add a row to the table when you've found a cause, not a guess, and say what
fixed it (a patch number, a profile setting, a recipe step). New techniques
go in the numbered section they belong to. Keep lab-specific details (your
console's address, your own scripts) out of this file: the repository is
public.
