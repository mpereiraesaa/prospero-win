# Telemetry contract

The title emits structured `ps5log/1` records over TCP, configured by the
private `dev.conf` the build copies into the package. Console filesystem and
USB logging are not evidence paths. Telemetry is part of the ownership
contract: it identifies the run, records progress and classifies how it
ended, without depending on a screenshot. Wine's own debug channels
(`WINEDEBUG`) reach the same stream through ntdll's output sink (patch 0560),
one record per line.

## Title records

Every record the title writes starts with `PW_WINE64`:

| Record | Carries |
| --- | --- |
| `args` | argument count, mode (`launcher`, `game`, `sync`), profile, cycle, refusal |
| `data_mount` | the `/data` request: present before, request written, errno, present after, wait |
| `library` / `profile refused` | profiles listed (by index or scan, with the scan errno), games, the library's root; each refused profile and why |
| `mirror` | the library copy for the launcher: target, status, errno |
| `launcher` / `launcher chose` | VideoOut and pad status, games, cycle; the chosen game or `sync` |
| `restart` / `restart failed` | the `LoadExec` target, cycle, reason and eboot; the failure code |
| `profile` | the game's id, prefix, desktop, scaling, view and input mode |
| `winedebug` | the game's own `WINEDEBUG`, when its profile's `[debug]` section sets one |
| `ntdll` / `load` / `environment` / `run` | the runtime found, `ntdll.prx` loaded (stage, module, segments), Wine's environment, `__wine_main` started |
| `display` / `audio` | the present sink, input and XInput hooks, VideoOut; the audio sink and port |
| `alive` | about once a second: Wine's output lines, frames delivered, shown and rejected with the last size, inputs posted and refused, the process's CPU time (`cpu_ms`, user and system: a stall that grows it by about 1000 a second spins, one that barely grows it waits), and the audio grains played (`audio`) and those with sound (`audible`); then Wine's VM call counters (`mmap=`) |
| `cpus` | the processors the system reports, which Wine gives the game as `NumberOfProcessors` |
| `fault_top` | every five seconds, when Wine's fault count grew: the pages that faulted most (Wine patch 0545), ranked, with the count, the 4 KiB page, the last faulting PC, the kind (0 read, 1 write, 8 execute), the page's and its host page's protection, and how it ended (1 resolved, 2 access violation, 3 other). One page at tens of thousands of faults a second is a loop, not a workload |
| `wine-ps5: wait snapshot` (from `WINESERVER`) | only in builds with `PW_WINE64_WAIT_WATCHDOG=1` (Wine patch 0680), every two seconds: each pending async with its thread, state and fd (`async`), each thread's message queue with its wake bits and masks, pending messages and any `SendMessage` it waits for or handles (`queue`), and each thread's current wait with its objects (`wait`, then one line per object); every thread's suspend counts and last request (`thread`), and for one silent outside any wait for over a second, its i386 registers and stack (`guest`) and the critical sections on that stack with their owners (`guest ... cs=`) |
| `wine-ps5: slow ...`, `wine-ps5: sleeper ...` (from `WINE`) | only with `PW_WINE64_WAIT_WATCHDOG=1` (Wine patch 0690): a server request answered after 200 ms, a lock taken after 100 ms, a thread stack or thread start slower than 100 ms, a direct-memory call over 50 ms (`slow`); a thread polling with `Sleep()` or asking for over half a second of sleep in two seconds, with its i386 stack (`sleeper`); a failed alert or a reused alert kqueue (`alert`) |
| `close requested` / `close timeout` | Options+Create (or the unattended deadline) sent Alt+F4; the game did not close in time |
| `rumble` | an XInput game's motor levels and the pad's result |
| `fault` | a fault before Wine's handlers: signal, address, RIP and the ntdll segment |
| `exit` / `done` | Wine ended the process; the start sequence's final status and stage |

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
