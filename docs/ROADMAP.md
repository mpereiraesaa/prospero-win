# Compatibility roadmap

This roadmap records public compatibility milestones and their evidence. It is
not a promise of dates or a list of internal experiments.

## Status vocabulary

- **Bring-up**: all required PE images map, relocate and bind, and guest
  execution reaches application initialization.
- **First frame**: the application presents its first recognizable frame.
- **In-game**: changing application graphics and required audio are present.
- **First playable**: the primary interaction loop works with native input.
- **Validated**: a repeatable hardware run satisfies its documented evidence
  gate and cleanup contract.

## Current baseline

### Hardware bootstrap

- PE32 images execute through an IA-32-to-x86-64 DBT with bounded W^X
  publication, hashed lookup, direct chaining, cross-block register residency,
  dead-flag elimination and lazy arithmetic flags.
- Reusable Win32, User32, GDI, registry, WinMM, Pad and callback foundations
  drive native AGC/VideoOut presentation, asynchronous SceAudioOut and
  `ps5log/1` telemetry.
- Space Cadet Pinball is **First playable** and hardware-validated as the first
  unmodified Windows target. It is a bootstrap and regression target, not the
  architecture's compatibility boundary.

### Wine bring-up

- The pinned i386 Wine revision is built reproducibly and recorded in a
  validated runtime manifest.
- The loader maps the real runtime graph, resolves exports by name and ordinal,
  follows bounded forwarders, and owns PE32 TLS plus minimal PEB, TEB and
  process-parameter state.
- All 256 pinned i386 Wine syscall IDs are cross-checked against source.
- Wine's syscall and Unix-call dispatchers are published from versioned tables
  cross-checked against the pinned source. The gate services 32 NT call shapes
  with validated guest spans and typed, monotonic handles.
- A generated PE32 executable plus two DLLs loads from the application
  namespace through Wine's own `ntdll`, reaches its own entry point, returns
  `1`, and exits cleanly through `NtTerminateThread`. All four chaining and
  residency combinations retire the same 598,404 guest instructions over the
  same 2,981 blocks and return the same status.
- The smaller ntdll control remains independently pinned at 33,367 retired
  instructions, 7,148 dispatches, 962 blocks and 19 serviced calls. Normal,
  sanitizer, pinned-source and cleanup gates pass.

Those Wine results are bounded host checkpoints. They do not yet mean that a
Wine process boots inside the PS5 title or that Pinball has migrated away from
the direct Win32 bootstrap.

## Current frontier

Cross-block residency is now a passing, measured path. Chaining reduces the
application gate's dispatcher returns by 86.9%; the first direct resident-ALU
emission and profitability filters reduce code and spill overhead, but the
allocator still has only three fixed host registers and no trace-wide view.
The loader graph and its load/memory/init lists still need read-back validation,
DllMain/TLS attach ordering is not yet evidence, and the current registry is
run-local rather than a persistent Wine prefix.

## Next compatibility release

1. Validate Wine-owned `PEB_LDR_DATA`, all loader lists and module identity,
   then prove dependency, DllMain and TLS attach/detach ordering.
2. Expand native process, thread, object and wait services: thread creation and
   exit, events, mutexes, semaphores, wait-any/wait-all, timeouts and abandoned
   ownership.
3. Complete the persistent prefix: `drive_c`, environment, registry views and
   crash-safe storage, DOS-device/NT path normalization, sharing and directory
   enumeration; prove two runs and two isolated prefixes.
4. Stage the pinned Wine distribution in the native title, boot the first Wine
   process on hardware and require structured identity, lifecycle and cleanup
   evidence.
5. Add a second independent PE32 application and selected Wine test subsets so
   application-specific assumptions cannot become runtime contracts.

The exact dependency graph and exit criteria live in
[WINE_FOUNDATION.json](WINE_FOUNDATION.json).

## Performance line

Compatibility work and DBT performance advance together, but neither
substitutes for correctness. Wine and multiple applications will drive:

- indirect-branch inline caches and return prediction;
- profile-guided resident allocation across compatible block contracts;
- broader SSE/SSE2 and integer instruction coverage;
- an intermediate representation with wider register allocation;
- bounded traces or superblocks with precise safepoints;
- thread-safe immutable translated-code reuse; and
- deterministic counters for dispatch, spills, exits, code size and frame
  pacing.

Host microbenchmarks and application gates guide optimization; PS5 performance
percentages require matched hardware measurements with exact state parity.

## 3D applications

The Direct3D path is DXVK, not a new prospero-win D3D renderer. DXVK's
D3D8/9/10/11 and DXGI PE modules will execute inside the same Wine runtime and
use `ps5-vulkan`, which owns AGC/VideoOut/gfx1013. Direct3D work begins when the
companion backend satisfies its pinned DXVK consumer profile.

This milestone requires:

- Wine/DXGI loader, thread, object and presentation contracts;
- Vulkan resource, lifetime and synchronization behavior required by DXVK;
- shader/pipeline support exposed through the ps5-vulkan consumer gate;
- deterministic visual and telemetry evidence; and
- no dependency on proprietary shaders or SDK blobs.

## Later work

- Broader multimedia, networking and filesystem compatibility.
- Wine-compatible exceptions, multi-threading and synchronization.
- PE64/x86-64 imports, callbacks, ABI/unwind support and an isolation policy.
- Additional graphics APIs only after the D3D9 backend is stable.

## Non-goals

- DRM or anti-cheat bypass.
- Kernel drivers.
- Shipping proprietary executables, DLLs, shaders, captures or SDK material.
- Claiming compatibility from static imports or a single screenshot.
