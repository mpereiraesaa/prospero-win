# Telemetry contract

The native runtime emits structured `ps5log/1` records over TCP. Console
filesystem and USB logging are not runtime evidence paths. Telemetry is part of
the ownership contract: it identifies the artifact, records progress and
classifies cleanup or failure without depending on a screenshot.

## Runtime records

| Record | Carries |
| --- | --- |
| `PW_RUNTIME_BEGIN` | schema, title, profile-mode flag, fallback root basename and telemetry status |
| `PW_APP_PROFILE` / `PW_PREFIX_OPEN` | selected app identity and profile-owned persistent root |
| `PW_RUNTIME_READY` | bound imports, guest entry and mapped bytes |
| `PW_STATE_LOAD` / `PW_STATE_SAVE` | persistent state bytes, generation and atomic-write results |
| `PW_PAD_OPEN` / `PW_PAD_EVENT` / `PW_PAD_QUIT` | Pad ownership, physical edges and explicit quit |
| `PW_VIDEO_FRAME` | dimensions, changing frame hash, flips, submits and fence state; with `backend=vk-wsi` also swapchain slot, present token, placement and staging hash |
| `PW_PRESENT_OPEN` / `PW_PRESENT_FAIL` | ps5-vulkan WSI backend, status, fixed extent and first failing Vulkan call |
| `PW_AUDIO_OPEN` / `PW_AUDIO_QUEUE` | PCM format, worker ownership, queue depth and completion |
| `PW_RUNTIME_HEARTBEAT` | DBT, pacing, adapter, renderer, audio and input counters |
| `PW_RUNTIME_TEARDOWN` | ordered release result plus RuntimeSupervisor session, outcome and cleanup state |
| `PW_RUNTIME_END` | normal reason, guest status and final counters |
| `PW_RUNTIME_ABORT` | fail-closed stage and status |
| `PW_RUNTIME_SIGNAL` | signal, fault address and native register context |

The production runner has no deadline and continues until the guest exits or
the operator closes it. `PW_TEST_EXIT_AFTER_MS` exists only for bounded
validation and follows the same teardown path.

## Acceptance rules

A live run requires rising heartbeats, matching title and artifact identity,
and no abort or signal. A completed finite run additionally requires ordered
teardown, `PW_RUNTIME_END` and the transport's matching `BYE` record.
When supervisor fields are present, finite-run validation also requires the
session to have completed, the supervisor to have returned to `IDLE`, and
cleanup to report `ok`. Profile-mode `PW_STATE_LOAD` / `PW_STATE_SAVE` paths
are isolated under that profile's prefix; they store Prospero-Win's current
registry snapshot, not Wine registry hives.

Renderer acceptance requires changing frame hashes, advancing flips/submits
and completed fence ownership. Audio acceptance requires advancing byte,
frame, block and completion counters; a successful port open is insufficient.
Physical input acceptance requires native Pad ownership and observed press and
release edges. A `PW_PRESENT_BACKEND=vk` run additionally requires
`--present-backend vk-wsi`: an open `PW_PRESENT_OPEN`, no `PW_PRESENT_FAIL`,
every sampled frame on `vk-wsi` with strictly increasing tokens over both
swapchain images and, for finite runs, a clean backend teardown whose flip
count matches `PW_RUNTIME_END`. Screenshots and videos are supporting evidence only.

The asynchronous audio contract additionally requires an active worker,
advancing enqueue/completion counts, zero output errors and bounded queue
high-water. `loop_gap_max_ns`, `loop_gaps_16ms` and `loop_gaps_33ms` report the
guest-thread pacing symptom. DBT telemetry reports dispatches, compiles,
cache hits/misses, hash probes and code-publication calls. Performance
comparisons require identical workloads and exact artifacts.

Validate a continuous transcript with:

```sh
python3 tools/validate_runtime_evidence.py run.log --continuous \
  --min-seconds 600 --min-flips 1000 --min-audio-blocks 800
```

For an operator gameplay run, add the physical input requirements:

```sh
python3 tools/validate_runtime_evidence.py run.log \
  --min-seconds 60 --min-flips 100 --min-audio-blocks 800 \
  --min-pad-events 12 --require-pad-quit
```

Omit `--continuous` for a bounded orderly-exit transcript. The validator
requires contiguous sequence numbers and cross-checks final work totals from
`PW_RUNTIME_END`; mutation tests ensure that missing or malformed evidence
cannot pass silently. See [hardware validation](HARDWARE_VALIDATION.md) for
the claims this contract supports.
