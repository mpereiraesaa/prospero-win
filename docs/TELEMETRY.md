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
| `ntdll` / `load` / `environment` / `run` | the runtime found, `ntdll.prx` loaded (stage, module, segments), Wine's environment, `__wine_main` started |
| `display` / `audio` | the present sink, input and XInput hooks, VideoOut; the audio sink and port |
| `alive` | about once a second: Wine's output lines, frames delivered, shown and rejected with the last size, inputs posted and refused; then Wine's VM call counters (`mmap=`) |
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
