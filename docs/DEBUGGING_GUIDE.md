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
8. When a game is slow, find out where its time goes before changing anything.

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

If a stall disappears as soon as you turn on Wine's channels, build the app
with `PW_WINE64_WAIT_WATCHDOG=1` instead ([what it logs](TELEMETRY.md)).
Every two seconds it shows what each thread waits on, in a few lines, so the
timing barely changes. A thread outside any wait is blocked in host code: the
snapshot then gives its i386 stack and the lock it waits on, with the lock's
owner. Resolve addresses with the `+loaddll` module bases and the PE files'
symbols.

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
  log says the game finished. To get past a game's menus into gameplay, see
  [Automated gameplay runs](#9-automated-gameplay-runs).
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

A random delay needs many runs: judge a change over ten launches or more.
Four good launches once looked like a fix and weren't.

"The PS5 can't do X" is a claim to measure before you write it down.

## 8. When a game is slow, time it first

A game that runs well on the PC but slowly on the PS5 can be slow in three
places: in its own code (run by the translator), in a host library it calls
through Wine (OpenGL, Vulkan, audio), or in Wine itself. The translator can
tell you which, without a profiler and at almost no cost.

- **On the PC,** set `PW_WOW_TIMING=1` for the run.
- **On the PS5,** where a game gets no environment variables, create an
  empty file named `pw_wow_timing` in the library folder
  (`/data/prospero-win/`) before starting the game, and delete it
  afterwards.

Every 5 to 10 seconds, each thread that calls out to the host often logs a
line (on the PS5 it reaches ps5log as a `WINESERVER` line):

```
wowprospero timing: tid=0024 run=16.5% unix=82.0% (310336/s 2.64us) sys=1.4% (1783/s 8.09us) other=0.0% (0) unix_over_1ms=0.0% (0) resets=0 flushes=0
```

- `run` is the share of the thread's wall time spent in translated code and
  the translator.
- `unix` is the time spent in Unix calls: graphics and audio libraries,
  reached through Wine. It's followed by how many calls there were per
  second and what each cost on average.
- `sys` is the same for system calls (Wine's server, waits, window
  management, `SwapBuffers`); a thread that mostly waits shows `sys` near
  100%.
- `unix_over_1ms` is the part of `unix` spent in calls that took over a
  millisecond. Those calls wait rather than work, usually for the display:
  a game held at 60 fps shows here how much of each frame it has to spare.
- `resets` and `flushes` count how often the translations were discarded
  (the code cache filled up, or code was unloaded). Both should stay near
  zero while a game runs.

Compare the same scene on the PC and on the PS5. A game that is slow only
on the PS5 shows which part grew. Half-Life is an example. On the PC it ran
at about 75 fps with `run=67% unix=24% (1870000/s 0.13us)`. On the PS5 it
ran at 12 fps with the line above: the same 25,000 OpenGL calls per frame
cost 20 times more there. A small test program that times single OpenGL
calls then showed that the cost of crossing from Wine to the library was
the same on both; small `glBegin`/`glEnd` draws were the slow part, inside
the PS5's OpenGL driver. OpenGL games also log their frame rate, as
`PW_GL frames=<n> fps=<rate>` every five seconds, so a run needs no one at
the TV to read the counter.

Avoid sampling profilers based on `SIGPROF` on the PS5. Its threading
library delays a signal that arrives while a thread holds one of its locks,
and the delayed sample then lands in libkernel's `getcontext` instead of
where the thread was.

## 9. Automated gameplay runs

Most games stop at a menu: a message of the day, a team choice, "press any
key". A script build of the app can press those keys itself, so a run reaches
real gameplay with nobody at the TV, and the same run can be repeated after
every change. `tools/pw_gameplay_run.py` sets such a run up, waits for it,
reports it and puts the console back as it was.

### The pieces

- **A script build.** `tools/build_native.sh` with `PW_WINE64_SCRIPT=1`: the
  launcher opens the library's first game by itself. `PW_WINE64_SECONDS=<n>`
  closes the game after n seconds, counted from its start, so leave room for
  loading (a map load can take 20 seconds). Keep `PW_WINE64_SCRIPT_CYCLES=1`
  for one game per run:

  ```sh
  PW_WINE64_SCRIPT=1 PW_WINE64_SECONDS=100 PW_WINE64_SCRIPT_CYCLES=1 tools/build_native.sh
  ```

  Install it in place of the app's `eboot.bin` for the run, and put the
  regular one back afterwards.
- **A key script.** A script build presses the keys listed in
  `pw_script_keys` in the library folder, one
  `<milliseconds after the game starts> <Windows virtual-key code>` per line,
  each as a press and a release, through the same path as a USB keyboard.
- **The saved log.** Every run is saved on the console in
  `/data/prospero-win/logs` (see [Getting a log](GETTING_STARTED.md#getting-a-log)),
  so the result can be read afterwards without a PC listening.

### Running one

`pw_gameplay_run.py` takes the game's profile name and the console's address
(`--host` or `PS5_HOST`), and for the run:

- lists only that game in `profiles.lst`, so the script build opens it;
- writes the key script from `--key MS:KEY` options (`enter`, `esc`, `space`,
  `tab`, arrows, `f1`–`f12`, digits, letters, or a code such as `0x0d`);
- `--arguments` replaces the game's launch arguments, for a game whose
  profile starts at its menu;
- `--append PATH=LINE` adds a line to a file under the library folder, such as
  a game's config;
- `--timing` turns on the [timing report](#8-when-a-game-is-slow-time-it-first);
- `--fps` turns on Wine's `fps` channel in the game's profile, for a game
  that draws with Vulkan through DXVK. OpenGL games log their frame rate
  without it (`PW_GL`).

Then start the script build on the console. When the game's session ends,
the tool copies its saved log to `--save` and summarizes it, and it always
puts back `profiles.lst`, the profile and the files it appended to, and
removes the key script and the timing trigger.

Counter-Strike 1.6 with nine bots, joining the counter-terrorists:

```sh
PS5_HOST=<PS5 IP> python3 tools/pw_gameplay_run.py counter-strike-16 \
  --arguments '-steam -game cstrike -noipx -window -w 1920 -h 1080 -gl +maxplayers 10 +map de_dust2' \
  --append 'prefix/drive_c/Games/CounterStrike16/cstrike/listenserver.cfg=bot_join_after_player 0' \
  --append 'prefix/drive_c/Games/CounterStrike16/cstrike/listenserver.cfg=bot_quota 9' \
  --key 25000:enter --key 25700:2 --key 26400:2 \
  --key 35000:enter --key 35700:2 --key 36400:2 \
  --timing --save runs/
```

ENTER closes the message of the day, the first 2 picks the team and the
second 2 the model. The second round of presses is there in case the map took
longer to load; in game they only switch weapons. On the console this printed:

```
pw_gameplay_run: keys sent: 6 (0xd:0/0, 0x32:0/0, 0x32:0/0, 0xd:0/0, 0x32:0/0, 0x32:0/0)
pw_gameplay_run: fps: 2.5 22.1 57.4 49.9 53.9 59.9 60.0 59.9 59.9 60.0 59.9 59.9
pw_gameplay_run: fps after loading: average 58.1, minimum 49.9, at 59 or more 7/9
pw_gameplay_run: timing: tid=0024 run=24.3% unix=66.0% (26514/s 24.90us) sys=9.6% ...
pw_gameplay_run: ended: close-timeout
```

The first three frame-rate samples cover loading and are left out of the
average. With `--fps`, Wine reports a Vulkan game's frame rate about every
1.5 seconds; samples before the first one at 20 fps or more are the loading
screen, and are left out. `ended` is how the session finished; `close-timeout` means the
script build asked the game to close and closed it when it didn't.

### Things to know

- **Find the keys on the PC first.** Play the game in Wine on the PC and note
  the keys and the seconds it takes to reach each menu; then add a few
  seconds, since loading is slower on the console.
- **Press the keys twice** a few seconds apart when a menu's timing varies,
  and pick keys that are harmless if the menu is already gone.
- **A helper program can't press the keys.** Wine on the console runs one
  process and can't start a second, so a launcher `.exe` that starts the game
  and sends keys fails; the script build has to do it.
- **Some settings only work from a config file.** Counter-Strike takes
  `bot_quota` only once a map's game code has loaded, so on the command line
  it does nothing; `listenserver.cfg` runs at the right time.
- **Compare like with like.** A run that starts a map and joins a team draws
  a different scene from one that sits at the menu or watches as a spectator;
  keep the same keys and arguments when you compare two builds.

## Symptoms we've seen

What each looked like, and what it turned out to be. Newest first.

| Symptom | Cause | Fix |
| --- | --- | --- |
| A Direct3D game through DXVK (Half-Life 2) showed no mouse pointer in its menus | Vulkan frames reach the screen through the GPU driver's own video output (patch 0460), which the PS5 user driver doesn't compose, so nothing drew the Windows cursor over them. A first fix scaled the position to the 3840x2160 output, which put the pointer at twice its place: VideoOut's cursor position is in the shown frame's pixels | Patch 0740 and `pw_videoout_cursor` in `libvulkan.prx`: after each present, the application's cursor goes to VideoOut's hardware cursor (two 64x64 images in display memory), at the desktop's coordinates. It's hidden while the game hides the cursor and while GDI frames show |
| Half-Life 2 took about 95 s to load a map. `srvbench`, a small test program, measured 5.2 ms for a `MapViewOfFile` + `UnmapViewOfFile` pair on the console against 0.1 ms on the PC, while the kernel's own `mmap`/`munmap` took about 3 µs | Wine tells the CPU backend only the address of a view being unmapped, and wowprospero flushed with no size, which discards every thread's translations. A 32-bit DXVK maps and unmaps windows of its texture memory about 12,000 times while Half-Life 2 loads a map, so every thread kept retranslating its code | The backend takes the view's extent just before the unmap and flushes only that range: 0.1–0.15 ms a pair on the console, and `d1_canals_01` loads in 25 s |
| Half-Life 2 froze while loading its first map, and closing it left the console stuck on "Closing…" until a reboot. The wait snapshot showed the main thread inside `CloseHandle`, called from DXVK's `d3d9.dll` | With no `memfd` on the PS5, Wine's server backed every anonymous section with a temp file on `/data`. A 32-bit DXVK keeps managed textures in 64 MiB anonymous sections that it creates and closes constantly (1,401 while Half-Life 2 loads its first map), so texture data went to storage, and closing one of those files blocked for good | Patch 0730: anonymous sections are anonymous shared memory (`shm_open(SHM_ANON)`). A test program repeating DXVK's pattern now closes each one in about 0.2 ms, and Half-Life 2 loads the map |
| An OpenGL game's menu showed no mouse pointer; its items still lit up under it | The user driver draws the cursor only over the GDI frames it composes; OpenGL frames reach the screen through the SDK's EGL surface. A first fix uploaded the cursor as `GL_BGRA`, which the SDK accepted without an error and drew nothing from | Patch 0723: the presenter blends the application's cursor over each OpenGL frame, uploaded as RGBA |
| Half-Life (OpenGL) ran at 12 fps on the PS5 and about 75 fps on the PC; OpenArena reached 60 | `PW_WOW_TIMING`: 82% of the game's main thread in OpenGL calls, 2.64 µs each against 0.13 µs on the PC. Each small `glBegin`/`glEnd` draw costs about 58 µs in the PS5's OpenGL driver against 6 µs on the PC, and Half-Life issues thousands per frame | The driver fetched `sceAgcGetRegisterDefaults()`, about 9 µs a call, on every draw: now once. `opengl32` replays per-vertex calls in one crossing (patch 0720). Mesa draws a run of `glBegin`/`glEnd` blocks as one triangle list, and no longer flushes on `glActiveTexture`. Half-Life holds 60 fps with about a third of each frame to spare |
| Warcraft III's intro started 0.2–32 s late, or never; the app idle; logging or a key press made it go away | Lock starvation in Wine's DirectShow ([#250](https://github.com/mpereiraesaa/prospero-win/issues/250)): `GetState()` held the renderer's lock while polled every 10 ms, and the PS5's ~1 ms thread wake-up always lost the race for it. Code that re-takes a lock right after releasing it can starve waiters here, not on Linux | Patch 0700: renderers wait without the lock (intro 0.19 s late, 6/6) |
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
