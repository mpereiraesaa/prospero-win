# Telemetry contract

The title emits structured `ps5log/1` records over TCP, configured by the
private `dev.conf` the build copies into the package, and saves the same
records on the console (see [Saved sessions](#saved-sessions)). Telemetry is part of the ownership
contract: it identifies the run, records progress and classifies how it
ended, without depending on a screenshot. Wine's own debug channels
(`WINEDEBUG`) reach the same stream through ntdll's output sink (patch 0560),
one record per line.

## Title records

Every record the title writes starts with `PW_WINE64`:

| Record | Carries |
| --- | --- |
| `args` | argument count, mode (`launcher`, `game`, `sync`), profile, cycle, refusal |
| `data_mount` | helper completion/error, `/data` before and after, wait, settle delay |
| `library` / `profile refused` | profiles listed (by index or scan, with the scan errno), games, the library's root; each refused profile and why |
| `mirror` | the library copy for the launcher: target, status, errno |
| `launcher` / `launcher chose` | VideoOut and pad status, games, cycle; the chosen game or `sync` |
| `restart` / `restart failed` | the `LoadExec` target, cycle, reason and eboot; the failure code |
| `profile` | the game's id, prefix, desktop, scaling, view, whether it shows the frame rate (`show_fps`) and input mode |
| `winedebug` | the game's own `WINEDEBUG`, when its profile's `[debug]` section sets one |
| `debug_env` | the names of the profile's `[debug] env` variables, comma-separated, when it has any |
| `runtime` / `fast_clock` | the profile's `[runtime]` switches (`thread_scheduling`, `shared_input`); with `fast_clock = true`, whether the TSC clock is on, the measured `tsc_hz` and the calibration result (`ok`, or why it was refused: `bracket`, `short`, `backward`, `range`, `disagree`) |
| `cpu` | a 32-bit game's CPU backend, `native` (with the Vulkan batching) or `translator`, and `prefix_cpu`: `1` copied `wow64native.dll` into the prefix, `0` it was already there, `-1` not copied (the prefix's own CPU runs), `-2` not needed |
| `ntdll` / `load` / `environment` / `run` | the runtime found, `ntdll.prx` loaded (stage, module, segments), Wine's environment, `__wine_main` started |
| `display` / `audio` | the present sink, input and XInput hooks, VideoOut; the audio sink and port |
| `alive` | about once a second: Wine's output lines, frames delivered, shown and rejected with the last size, inputs posted and refused, the process's CPU time (`cpu_ms`, user and system: a stall that grows it by about 1000 a second spins, one that barely grows it waits), and the audio grains played (`audio`) and those with sound (`audible`); then Wine's VM call counters (`mmap=`) |
| `cpus` | the processors the system reports, which Wine gives the game as `NumberOfProcessors` |
| `fault_top` | every five seconds, when Wine's fault count grew: the pages that faulted most (Wine patch 0545), ranked, with the count, the 4 KiB page, the last faulting PC, the kind (0 read, 1 write, 8 execute), the page's and its host page's protection, and how it ended (1 resolved, 2 access violation, 3 other). One page at tens of thousands of faults a second is a loop, not a workload |
| `memory` | every five seconds: the title's free flexible memory; the direct memory backing Wine's anonymous memory, now and at its peak (`dmem`, `dmem_peak`), in how many runs, and refused calls; Wine's heap, now and at its peak; and the part of that direct memory below 4 GiB, now and at its peak (`dmem_low`, `dmem_low_peak`). A 32-bit game's own memory lives below 4 GiB: 2 GiB of address space, or 4 GiB when its exe is large-address-aware. Wine's 64-bit side and the DBT live above it. `dmem_low` shows how close a game gets to its limit; `dmem` minus `dmem_low` is our own overhead |
| `wine-ps5: wait snapshot` (from `WINESERVER`) | only in builds with `PW_WINE64_WAIT_WATCHDOG=1` (Wine patch 0680), every two seconds: each pending async with its thread, state and fd (`async`), each thread's message queue with its wake bits and masks, pending messages and any `SendMessage` it waits for or handles (`queue`), and each thread's current wait with its objects (`wait`, then one line per object); every thread's suspend counts and last request (`thread`), and for one silent outside any wait for over a second, its i386 registers and stack (`guest`) and the critical sections on that stack with their owners (`guest ... cs=`) |
| `wine-ps5: slow ...`, `wine-ps5: sleeper ...` (from `WINE`) | only with `PW_WINE64_WAIT_WATCHDOG=1` (Wine patch 0690): a server request answered after 200 ms, a lock taken after 100 ms, a thread stack or thread start slower than 100 ms, a direct-memory call over 50 ms (`slow`); a thread polling with `Sleep()` or asking for over half a second of sleep in two seconds, with its i386 stack (`sleeper`); a failed alert or a reused alert kqueue (`alert`) |
| `PW_GL` (from `WINESERVER`) | every five seconds while an OpenGL game presents (Wine patch 0721): the frames shown in that time and the frame rate, e.g. `PW_GL frames=300 fps=60.0` |
| `wowprospero timing` (from `WINESERVER`) | only when `/data/prospero-win/pw_wow_timing` exists: every 5 to 10 seconds, each busy guest thread's split between translated code (`run`), Unix calls (`unix`) and system calls (`sys`), and the Unix calls that took over a millisecond, which are waits (see the [debugging guide](DEBUGGING_GUIDE.md#8-when-a-game-is-slow-time-it-first)) |
| `wowprospero calls` (from `WINESERVER`) | with each timing line: the system calls (`sys_top`) and Unix calls (`unix_top`) that took most of that thread's time, with their rate and share (see the [debugging guide](DEBUGGING_GUIDE.md#8-when-a-game-is-slow-time-it-first)) |
| `close requested` / `close timeout` | Options+Create (or the unattended deadline) sent Alt+F4; the game did not close in time |
| `rumble` | an XInput game's motor levels and the pad's result |
| `fault` | a fault before Wine's handlers, in hex: signal, address, RIP, and the ntdll segment holding RIP with the offset in it (`ntdll_segment=`, `offset=`, when RIP is inside ntdll). Written from the signal handler to the saved session and the live stream, as a raw line |
| `marker by=player` | the player pressed Create in a game (without Options), to mark a moment worth looking at in the log |
| `session_end` | the run's last record: why the session ended (`wine-exit`, `launcher`, a failed restart) |
| `exit` / `done` | Wine ended the process; the start sequence's final status and stage |

## Saved sessions

The title also writes every record to `<library root>/logs/session-N.log`
(`/data/prospero-win/logs` when the `/data` grant succeeds). Each launcher or
game run is a session; the eight newest are kept, and `next.txt` holds the
number the next session takes. A file starts with
`PW_REPORT/1 build=<commit> profile=<game or launcher> cycle=<n> pid=<pid> time=<epoch>`
and then has one line per record, `REC seq=<n> t=<monotonic seconds> <record>`.
A session keeps two chunks of at most a megabyte: when the current one is
full, it becomes `session-N.previous.log` (replacing the older chunk) and a new
one starts with the same `PW_REPORT/1` line. The title writes buffered records
every 100 ms and before every restart, so a crash loses at most that much.

Records longer than a `ps5log` record (1024 bytes) are cut and end in
`[truncated]`, in both the saved file and the live stream.

When the live channel drops, a background thread reconnects every five
seconds; the title's own loop never waits for the network. Records made
during a reconnect are saved but not sent.

## Acceptance rules

A game run is accepted when the stream is gap-free and names the title, the
`profile` record matches the chosen game, `alive` records keep coming with
rising shown frames, and the run ends by `exit` (Wine closed) or a
classified `close timeout`, followed by the transport's `BYE`. A `fault`
record or a missing `BYE` fails the run. Input acceptance requires posted
inputs (or XInput state) while a person or the unattended script uses the
pad; audio acceptance requires an open sink and port. A launcher run is
accepted when every `restart` it records is followed by the next
generation's `args` record with the expected mode and cycle.

Screenshots and videos are supporting evidence only. Performance comparisons
require identical workloads and exact artifacts.
