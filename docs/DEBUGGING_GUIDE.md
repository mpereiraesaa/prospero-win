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
9. Measure speed-ups on the PS5 itself, against main, one change at a time.

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

Performance is the exception. The PC's CPU, caches and graphics stack are
different enough that a speed-up there says little about the console, so
profile and compare builds on the PS5 directly; see
[profiling on the PS5](#10-profiling-on-the-ps5). It needs no Wine on the PC.

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

After each timing line, a `wowprospero calls` line names the calls that took
the most of that thread's time, one line for system calls and one for Unix
calls, as `number/calls per second/share of wall time`:

```
wowprospero calls: tid=0140 sys_top 0x105/170/s/20.9% 0x50/1573/s/1.8% ... dropped=0
wowprospero calls: tid=016c unix_top f4f0:730/99687/s/6.2% f4f0:55/20081/s/4.0% ... dropped=0
```

System call numbers are Wine's 32-bit ones (`dlls/ntdll/ntsyscalls.h`, and
`dlls/win32u/win32syscalls.h` from 0x1000). A few that matter:
`0x105` NtWaitForAlertByThreadId is a thread waiting for another thread (a
critical section, an SRW lock or a condition variable), `0x34`
NtDelayExecution is `Sleep` (a frame limiter spinning on `Sleep(0)` shows it
next to `0x31` NtQueryPerformanceCounter), and `0x4` NtWaitForSingleObject is
a wait on an event or a handle. A Unix call is shown as a library tag (bits 4
to 19 of its handle; the same for every call into one library) and the
library's function number; for winevulkan, the number is the position in
`enum unix_call` in `dlls/winevulkan/loader_thunks.h`, counted from 0.
`dropped` counts calls that found the per-thread table full; it should be 0.

In GTA San Andreas with Proper Shaders, these lines showed the main thread
spending a fifth of its time in `0x105`: it was waiting for DXVK's
command-stream thread, which a per-frame read-back of one pixel forced it to
drain.

Compare the same scene on the PC and on the PS5. A game that is slow only
on the PS5 shows which part grew. Half-Life is an example. On the PC it ran
at about 75 fps with `run=67% unix=24% (1870000/s 0.13us)`. On the PS5 it
ran at 12 fps with the line above: the same 25,000 OpenGL calls per frame
cost 20 times more there. A small test program that times single OpenGL
calls then showed that the cost of crossing from Wine to the library was
the same on both; small `glBegin`/`glEnd` draws were the slow part, inside
the PS5's OpenGL driver. That driver, the PS5 OpenGL SDK, has since been
replaced by Mesa's Zink on Vulkan. Its builds logged the frame rate as
`PW_GL frames=<n> fps=<rate>` every five seconds; with Zink, read Wine's
`fps` channel instead (`pw_gameplay_run.py --fps`), as for DXVK.

Avoid sampling profilers based on `SIGPROF` on the PS5. Its threading
library delays a signal that arrives while a thread holds one of its locks,
and the delayed sample then lands in libkernel's `getcontext` instead of
where the thread was.

## 9. Automated gameplay runs

Most games stop at a menu: a message of the day, a team choice, "press any
key". A script build of the app can press those keys, or controller buttons,
itself, so a run reaches
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
  so the retained result can be read afterwards without a PC listening.
  The console keeps only the latest two 1 MiB chunks of each session. For a
  long or noisy run, start `pw_gameplay_run.py` before gameplay: it accumulates
  records during polling and saves them with `--save`, including chunks that
  have since rotated away. Records are deduplicated by sequence within the
  same session identity; partial lines wait for a later poll, and raw fault
  records are retained. A capture over 64 MiB is refused rather than trimmed.
  Polling interruptions or rotation faster than polling can still lose data.
  The summary reports the retained time span and internal sequence gaps;
  a gap-free tail alone does not prove that the beginning was captured.
  Compare only matching gameplay windows actually present in both logs;
  the general FPS summary does not select a fixed benchmark window.

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
- `--timing` turns on the [timing report](#8-when-a-game-is-slow-time-it-first),
  and `--profiler` the translator's other profilers;
- `--runtime` runs the game with another build of the translator, put back
  afterwards, and `--fetch` copies a file such as the game's own benchmark
  log into `--save` (both in [profiling on the PS5](#10-profiling-on-the-ps5));
- `--fps` turns on Wine's `fps` channel in the game's profile, for a game
  that presents with Vulkan: DXVK, or Zink for OpenGL. Logs from builds with
  the old PS5 OpenGL SDK carry `PW_GL` lines instead, which it still reads.
- `--input FILE` replays a recorded macro: keys held and released, mouse
  buttons and relative mouse motion, through the same path a USB keyboard
  and mouse take, and controller buttons and sticks. One event per line, its
  time in milliseconds:
  - `<ms> key <virtual-key code> <1|0>` (1 presses, 0 releases);
  - `<ms> button <0|1|2> <1|0>` (left, right or middle mouse button);
  - `<ms> move <dx> <dy>` (mouse motion);
  - `<ms> pad <button> <1|0>`, a controller button: `a`, `b`, `x`, `y`,
    `start`, `back`, `lb`, `rb`, `ls`, `rs` (stick clicks), `up`, `down`,
    `left`, `right` (the d-pad), `guide`, or an XInput button mask such as
    `0x1010` for A and Start together;
  - `<ms> stick <l|r> <x> <y>`, a stick held at a position from -32768 to
    32767 (y points up); `stick l 0 0` lets it go.

  A controller button stays down, and a stick where it was put, until a
  later line changes it. The game sees this controller as its first Xbox
  controller, so it only works for games whose profile has
  `[input] mode = xinput`. It's added to the real DualSense rather than
  replacing it, so you can still play during the run: buttons from either
  count as pressed, and a stick the macro holds off centre takes over from
  yours until it lets go.

  An optional first line `sync <path under the library folder> <text>`
  starts the clock when that text appears in what the file gains after the
  game starts. For Half-Life 2 that's its `-condebug` log,
  `hl2/console.log`, and `Redownloading all lightmaps`, printed once a level
  has loaded. The summary says how many events were replayed.
- `--pad MS:BUTTON[:HOLDMS]` presses a controller button MS milliseconds
  after the game starts and holds it for HOLDMS milliseconds (150 if you
  leave it out), using the button names above. It writes the press and the
  release into the macro, merged with `--input`'s lines by time if you give
  both; with a `sync` line, MS counts from the sync instead. The summary
  lists when the game's controller buttons changed.

Then start the script build on the console. When the game's session ends,
the tool copies its saved log to `--save` and summarizes it. It always puts
back `profiles.lst`, the profile, the files it appended to and the app's own
translator, and removes the key script and any profiler files.

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
- **Use the controller when a menu ignores keys.** Some menus don't respond
  well to keys. On the console, an ENTER from `--key` (a press and a release
  in the same instant) only sometimes chose PLAY in GTA IV's episode
  selector, while the DualSense's Cross button worked every time. For a
  game in xinput mode, press its controller button instead:

  ```sh
  PS5_HOST=<PS5 IP> python3 tools/pw_gameplay_run.py gtaiv --pad 45000:a --save runs/
  ```

  Hold a button for at least a few frames; the default 150 ms suits a game
  that checks its controller once a frame. The script build keeps even a
  zero-length press down for one of its own frames, but a game that checks
  less often can still miss a press that short.
- **A helper program can't press the keys.** Wine on the console runs one
  process and can't start a second, so a launcher `.exe` that starts the game
  and sends keys fails; the script build has to do it.
- **Some settings only work from a config file.** Counter-Strike takes
  `bot_quota` only once a map's game code has loaded, so on the command line
  it does nothing; `listenserver.cfg` runs at the right time.
- **Compare like with like.** A run that starts a map and joins a team draws
  a different scene from one that sits at the menu or watches as a spectator;
  keep the same keys and arguments when you compare two builds.

## 10. Profiling on the PS5

Every profiler in the translator also runs on the console. You can find out
where a game spends its time, and check whether a change to the translator
makes it faster, on the PS5 itself, without running the game in Wine on a PC.
None of this is specific to one game: it works for any 32-bit Windows game
the app runs, because every one of them runs through the translator. (A
64-bit game runs natively and doesn't use the translator, so these profilers
have nothing to report for it.)

The steps below use two tools that run on your PC and talk to the console
over FTP: `tools/pw_gameplay_run.py`, which sets up one unattended run and
puts everything back afterwards ([section 9](#9-automated-gameplay-runs)),
and `tools/pw_exec_cpu.py`, which reads the saved logs and compares builds.

### What you need

- The app installed on the PS5, with FTP access to the console, as in
  [Getting started](GETTING_STARTED.md).
- A **script build** of the app, so a run starts the game and ends it
  without anyone at the TV
  ([the pieces](#the-pieces)). Set `PW_WINE64_SECONDS` long enough for the
  game to load and finish what you are measuring.
- To compare translator changes, **two builds of the translator**. Each
  build of the runtime (`tools/build_wine_ps5.sh`) writes the signed module
  as `prx/sce_module/wowprospero.prx` in its work directory
  (`.deps/wine-ps5/` by default). Build `main`, copy that file aside as
  the baseline, then build your change and copy that one aside too.

### The profilers

Each profiler is switched on by a file in the library folder
(`/data/prospero-win/`) that the translator looks for when a game starts.
`pw_gameplay_run.py --profiler NAME` creates the file for one run and removes
it afterwards; you can name more than one.

| `--profiler` | File | What it reports |
| --- | --- | --- |
| `timing` | `pw_wow_timing` | Where each thread's time goes: translated code, host libraries, system calls ([section 8](#8-when-a-game-is-slow-time-it-first)) |
| `exec-timing` | `pw_wow_exec_timing` | Sampled CPU-time estimates inside translated code, per thread |
| `profile` | `pw_wow_profile` | The 40 translated blocks each thread spends the most samples in, every five seconds, and every minute each block with at least two samples (`hotcum` lines) |
| `dispatch-profile` | `pw_wow_dispatch_profile` | How often a jump between blocks finds its target in the translator's chain table, finds the slot empty, or finds another block there |

Two files switch translator features off for a comparison run:
`pw_wow_no_jump_tables` keeps the table lookup for switch statements, and
`pw_wow_no_jump_predict` keeps it for import thunks and `call [import]`
(see [the benchmark log](DBT_BENCHMARK.md#switches-import-thunks-and-call-returns)).

They report through the game's log, which the console saves after every run
and `pw_gameplay_run.py --save DIR` copies to your PC. If you start a game
by hand instead, you can create and delete the files over FTP yourself;
leave nothing behind, since some of them slow every game down.

### Step 1: find where the time goes

Start with `--profiler timing`. If most of a busy thread's time is in `unix`
or `sys`, the translator is not the problem; read
[section 8](#8-when-a-game-is-slow-time-it-first). If it's in `run`, add
`--profiler profile` to see which code:

```
wowprospero profile: tid=0024 interval_ms=6405 samples=123 outside=0 stubs=9 overflow=0
wowprospero hotspot: tid=0024 pc=7a7b3c33 samples=6 entry=0 body=0 exit=6 emitted=0
```

`pc` is the guest address of a translated block, which you can look up in
the game's DLLs. `entry`, `body` and `exit` say which part of the translated
block the samples landed in; samples in `exit` are time spent getting to the
next block, and `--profiler dispatch-profile` then shows whether blocks are
evicting each other from the chain table (`table_collisions`). These are
statistical samples, not exact timings, and they only cover translated code:
the caveat about signals at the end of
[section 8](#8-when-a-game-is-slow-time-it-first) applies to the rest.

### Step 2: choose a workload that repeats exactly

A comparison is only as good as its workload. Use something the game does
the same way every time:

- **A built-in benchmark.** Many engines can play back a recorded demo and
  report how long it took. In Source and GoldSrc games (Half-Life 2,
  Half-Life, Counter-Strike), record a demo of the stretch you care about in
  the game's console (`record <name>`, play, `stop`), then start the run
  with `--arguments` set to the game's usual launch arguments plus
  `-condebug +timedemo <name>`. `-condebug` makes the game write its
  console, result included, to a log in its game folder (`console.log` in
  Source games, `qconsole.log` in GoldSrc ones), which you copy back with
  `--fetch` and its path under the library folder, such as
  `prefix/drive_c/Games/<game>/<mod>/console.log`. Quake III engine games such as OpenArena have
  the same idea as `timedemo 1` followed by `demo <name>`.
- **A recorded macro.** Otherwise, record the inputs that play through one
  stretch of the game (`--input`, [running one](#running-one)) and replay
  the same file every time, with the same settings.

Keep the game's settings, resolution and launch arguments identical for
every run you compare.

### Step 3: compare two builds

Most games on the console are held at 60 fps by the display, so a faster
translator usually doesn't raise the frame rate: it leaves spare CPU. That
is why the comparison measures CPU time with `--profiler exec-timing`.

Run the baseline and your change in turn: baseline, change, baseline,
change, baseline. Measure the variation in your own control runs; there is
no fixed drift percentage. Alternating helps expose drift instead of giving
one build all the early or late runs. `--runtime` installs a translator build for the run
and puts the app's own back afterwards; `--app` is the app's folder on the
console, `/data/homebrew/` followed by the folder name you uploaded. For a
game whose profile is `my-game` with a recorded demo `mydemo`:

```sh
export PS5_HOST=<PS5 IP>
for build in baseline change baseline change baseline; do
  python3 tools/pw_gameplay_run.py my-game \
    --arguments '<the game arguments> -condebug +timedemo mydemo' \
    --fetch 'prefix/drive_c/Games/<game>/<mod>/console.log' \
    --runtime prx/$build/wowprospero.prx --app /data/homebrew/<app folder> \
    --profiler exec-timing --save runs/$build/
done
python3 tools/pw_exec_cpu.py --baseline runs/baseline/*.log --candidate runs/change/*.log
```

Start the script build on the console each time the tool says it's ready.
The comparison prints one line per busy thread:

```
tid=0024: baseline 97.0, 98.0 s; candidate 94.0, 95.0 s; median -3.1%: faster
tid=00b0: baseline 71.5, 71.4 s; candidate 69.0, 69.1 s; median -3.4%: faster
```

This illustrative output assumes zero clock errors on every compared
thread. The seconds estimate CPU inside translated invocations over the
whole captured session, including loading, demo warmup and post-demo work.
They are not CPU time for just the measured frames. Each thread logs a line like this one:

```
wowprospero execution: tid=0024 cumulative=1 sample_cpu_ns=1508594000 calls=32265545 samples=504504 stride=64 clock_batch_read_ns=852 clock_resolution_ns=1000 clock_errors=0
```

Only about one call into translated code in 64 is timed, so the total is
`sample_cpu_ns / samples * calls`: here 1.508594 s / 504,504 × 32,265,545,
about 96.5 s. Clock-read overhead is included in the samples and expanded
by this formula too; changes in invocation count can change that overhead.
Keep the instrumentation identical and do not subtract the reported batch
clock-read cost as though it were an exact correction. `pw_gameplay_run.py` also prints the busiest threads' totals
after every run, as `translated CPU:`.

How to read the verdict:

- **faster** or **slower** means every run of one build beat every run of
  the other in the sampled whole-session estimates. This is a screening
  result, not proof of a benchmark-phase gain. Anything in between is
  **no clear difference**, however good the average looks. For a phase claim,
  bracket the workload with actual counter records from the same thread and
  process; the demo's duration alone cannot locate those records.
- Compare thread by thread. Thread numbers (`tid`) are usually the same from
  run to run for a game's busiest threads; the tool only compares threads
  that appear in every run.
- Any nonzero `clock_errors` count makes that thread's estimate unreliable;
  a missing count is unknown and is also marked unreliable. It is unsafe
  to assume a small count is harmless. Keep those errors visible and rerun
  before using that thread to claim a gain.
- Check all significant threads and process CPU as well. Matching thread
  numbers alone does not prove identical work or exhaustive coverage, and
  process CPU includes native work outside the translated-invocation clock.
- Build each change on `main` by itself. A branch that also carries other
  experiments measures all of them at once. Once, several candidate branches
  all showed the same large gain, and it came from a single commit they
  shared.

### Step 4: play it, and quit

A benchmark doesn't exercise everything. Before you call a change done, play
through a real stretch of the game with it (a recorded macro is fine), quit
from the game's own menu, and check the summary: `ended: wine-exit` means
the game and Wine exited normally. `close-timeout` means the script build
had to close the game, so the quit wasn't tested. Look through the saved log
for translator errors too.

### Example: Half-Life 2

The full-PC chain hash, shared fault exit and second chain-table bank were
merged in October 2026 after profiling, comparisons and gameplay checks.
The workload was a long demo recorded in
`d1_trainstation_01`, played with `+timedemo` (5,182 frames, at 59.8 fps on
every build because of the 60 Hz cap). `-condebug` in the launch arguments
makes the game write its console, including the timedemo result, to
`hl2/console.log`. The dispatch profile showed blocks evicting each other
from the chain table, which is what the first and third changes address,
and the comparisons were then run as in step 3, followed by a macro playing
from the train to Barney's monitor and quitting. These historical comparisons
had incomplete phase boundaries and some nonzero clock-error counts, so
they do not prove exact CPU savings. The second bank is retained for its
consistent reduction in dispatcher entries and passed gameplay checks;
its reported 2–3% CPU saving remains unproven. The full numbers, and the
candidates that showed no gain, are in the
[DBT benchmark notes](DBT_BENCHMARK.md).

### Put the console back

`pw_gameplay_run.py` restores the files it changes, even when a run fails or
is interrupted: the game list, the profile, the translator and the profiler
files. It can't undo what the game itself saved during the run. Check the
game's own settings afterwards: Half-Life 2, for example, writes
`joystick "0"` into `hl2/cfg/config.cfg` when it starts without a gamepad,
and the next person to play finds their controller dead.

## Symptoms we've seen

What each looked like, and what it turned out to be. Newest first.

| Symptom | Cause | Fix |
| --- | --- | --- |
| Every game start logged `cannot open .../winspool.so, tried .../winspool.prx` and then `syscall fault 0xc0000005 at .../ntdll.prx+0x2d045, address 8`. Wine caught it and the game ran normally | The printer module, `winspool.drv`, has a Unix side that talks to CUPS, and the console runtime has none. Wine still loads the system printers when the module attaches, and its first call went through the missing library's empty call table, at slot 1, offset 8. Any Wine module whose `.prx` is missing on the console can fault this way | Patch 0750: `winspool.drv` returns an ordinary "not found" error when its Unix side is missing, so it carries on as a system with no printers. On the console the fault is gone; Half-Life 2 started, played its route at 60 fps and exited normally |
| Half-Life 2 ran at 60 fps, then a couple of minutes into the train station fell to between 1 and 15 fps for up to a few minutes and recovered on its own. Every speed-up to the translated code made no difference, and the process used *less* CPU while it was slow | The translation cache finds a block's code in a hash table that only empties when the cache resets. A lookup that misses scans until it reaches an empty slot, so as the main thread's 65,536-entry table filled, every miss walked long runs of entries. That scan, in `pw_x86_cache_lookup_mut`, took most of the CPU while the game itself barely ran, until the table filled up and a reset cleared it. A title build that logged the module map showed the profiler's hottest native address was that loop | The cache now resets at three quarters full, before lookups slow down (#312), and the game's first thread gets 131,072 entries so its hot code, about 50,000 to 60,000 blocks, fits without resetting every minute (#316). The train-station route now holds 60 fps throughout |
| A Direct3D game through DXVK (Half-Life 2) showed no mouse pointer in its menus | Vulkan frames reach the screen through the GPU driver's own video output (patch 0460), which the PS5 user driver doesn't compose, so nothing drew the Windows cursor over them. A first fix scaled the position to the 3840x2160 output, which put the pointer at twice its place: VideoOut's cursor position is in the shown frame's pixels | Patch 0740 and `pw_videoout_cursor` in `libvulkan.prx`: after each present, the application's cursor goes to VideoOut's hardware cursor (two 64x64 images in display memory), at the desktop's coordinates. It's hidden while the game hides the cursor and while GDI frames show |
| Half-Life 2 took about 95 s to load a map. `srvbench`, a small test program, measured 5.2 ms for a `MapViewOfFile` + `UnmapViewOfFile` pair on the console against 0.1 ms on the PC, while the kernel's own `mmap`/`munmap` took about 3 µs | Wine tells the CPU backend only the address of a view being unmapped, and wowprospero flushed with no size, which discards every thread's translations. A 32-bit DXVK maps and unmaps windows of its texture memory about 12,000 times while Half-Life 2 loads a map, so every thread kept retranslating its code | The backend takes the view's extent just before the unmap and flushes only that range: 0.1–0.15 ms a pair on the console, and `d1_canals_01` loads in 25 s |
| Half-Life 2 froze while loading its first map, and closing it left the console stuck on "Closing…" until a reboot. The wait snapshot showed the main thread inside `CloseHandle`, called from DXVK's `d3d9.dll` | With no `memfd` on the PS5, Wine's server backed every anonymous section with a temp file on `/data`. A 32-bit DXVK keeps managed textures in 64 MiB anonymous sections that it creates and closes constantly (1,401 while Half-Life 2 loads its first map), so texture data went to storage, and closing one of those files blocked for good | Patch 0730: anonymous sections are anonymous shared memory (`shm_open(SHM_ANON)`). A test program repeating DXVK's pattern now closes each one in about 0.2 ms, and Half-Life 2 loads the map |
| An OpenGL game's menu showed no mouse pointer; its items still lit up under it | The user driver draws the cursor only over the GDI frames it composes; OpenGL frames reach the screen through the SDK's EGL surface. A first fix uploaded the cursor as `GL_BGRA`, which the SDK accepted without an error and drew nothing from | Patch 0723 (removed with the PS5 OpenGL SDK; Zink frames get VideoOut's hardware cursor, patch 0740): the presenter blends the application's cursor over each OpenGL frame, uploaded as RGBA |
| Half-Life (OpenGL) ran at 12 fps on the PS5 and about 75 fps on the PC; OpenArena reached 60 | `PW_WOW_TIMING`: 82% of the game's main thread in OpenGL calls, 2.64 µs each against 0.13 µs on the PC. Each small `glBegin`/`glEnd` draw costs about 58 µs in the PS5's OpenGL driver against 6 µs on the PC, and Half-Life issues thousands per frame | The driver fetched `sceAgcGetRegisterDefaults()`, about 9 µs a call, on every draw: now once. `opengl32` replays per-vertex calls in one crossing (patch 0720). Mesa draws a run of `glBegin`/`glEnd` blocks as one triangle list, and no longer flushes on `glActiveTexture`. Half-Life holds 60 fps with about a third of each frame to spare |
| Warcraft III's intro started 0.2–32 s late, or never; the app idle; logging or a key press made it go away | Lock starvation in Wine's DirectShow ([#250](https://github.com/mpereiraesaa/prospero-win/issues/250)): `GetState()` held the renderer's lock while polled every 10 ms, and the PS5's ~1 ms thread wake-up always lost the race for it. Code that re-takes a lock right after releasing it can starve waiters here, not on Linux | Patch 0700: renderers wait without the lock (intro 0.19 s late, 6/6) |
| The stick and USB mouse didn't move the pointer; keys worked. `inputs` grew, nothing refused | win32u (Wine 11) holds a driver's mouse motion until a button or `MOUSEEVENTF_MOVE_NOCOALESCE` sends it; the PS5 driver never sent it. Before, the pointer only seemed to move because the driver answered `GetCursorPos` itself | Wine patch 0670: relative motion, sent with `MOUSEEVENTF_MOVE_NOCOALESCE` |
| An RTS map didn't scroll at the screen's edges | The app sent absolute positions, and nothing while the pointer was held at an edge; the game's own `SetCursorPos` was overridden | Patch 0670: the app sends motion, Wine keeps the only pointer |
| A white rectangle while the game loaded | win32u fills a new window surface with white; a Direct3D window is never painted | Patch 0650: new surfaces start black on the PS5 |
| 800x600 cinematics in the top-left corner | The PS5 driver refused display mode changes | Patch 0630: accept modes up to the desktop's size and show the part they cover |
| The screen went black at 1440x1080 | VideoOut refuses that size | Use 1920x1080 (for Warcraft III, with RenderEdge for 16:9) |
| GDI frames shown unscaled after the Vulkan handover | The frames' pixel format; VideoOut only scales B8G8R8A8 | Draw GDI frames as B8G8R8A8 |
| Movies didn't play | No GStreamer on the PS5; also crypt32's Unix side was missing | LAV Filters in the prefix (winetricks `lavfilters`); build `crypt32.prx` |
| Battle.net's login page aborted (`int3; ud2` in `libcef.dll`) after `syscall fault ... ntdll.prx+0x2e1a5, address 30` (also `18`, `38`, `48`) | The unix-call dispatcher called through a NULL table: `dwrite.so` was not built, and the faulting addresses are 8 × DirectWrite's Unix call numbers (glyph advance, bounding box, metrics) | Build `dwrite.prx`; export the FreeType functions `dlls/dwrite/freetype.c` loads |
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
