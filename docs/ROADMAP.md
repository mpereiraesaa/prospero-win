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

Compiling the Wine distribution does not make it directly loadable on PS5.
The current reproducible build supplies Wine's PE32 modules and exact runtime
data. PE32 execution still needs the DBT, and Wine's Unix-facing boundary still
needs PS5-native VM, filesystem, thread, object, wait and lifecycle adapters.
Future PE64 execution can use native x86-64 instructions, but only after the
Windows calling convention, callbacks, exceptions, unwind state and loader
boundary are bridged to the native title ABI.

## Product direction: copy and run

The initial user flow is intentionally narrower than a Windows desktop:

1. Copy an already installed, independently runnable application directory
   into title-owned storage.
2. Register a small manifest containing the EXE, working directory, arguments,
   architecture, prefix identity and explicit DLL overrides.
3. Launch that EXE through the shared Wine runtime and its isolated prefix.
4. Preserve files and registry state so the same application relaunches without
   a PC, network connection or development configuration.

The copied application directory and its prefix are separate inputs. The
application supplies its executables and data; the prefix supplies `drive_c`,
Windows directories, environment, registry hives and per-application state.
Copying a game folder alone cannot recreate registry values written by an
installer, so compatibility recipes or a later direct-installer path may seed
those values when required.

Explorer is optional UI, not a runtime prerequisite. An installer EXE can
eventually be launched as directly as a game EXE. MSI, child-process trees,
service management and store clients belong after the single-process runtime
is reliable; none blocks portable installed-folder applications.

## Next compatibility release: native Wine copy-and-run

### 1. Close the Wine loader contract

- Validate Wine-owned `PEB_LDR_DATA`, load/memory/init lists and module
  identity.
- Prove dependency, `DllMain` and TLS attach/detach ordering, including failure
  unwind and clean process teardown.
- Keep the generated two-DLL application as the exact regression gate.

### 2. Boot Wine inside the PS5 title

- Link the PS5-native runner and platform adapters instead of the direct
  Pinball dispatcher.
- Stage the pinned i386 Wine distribution from its validated manifest.
- Boot the generated PE32 application through Wine `ntdll` under the DBT on
  hardware, reach its entry point, return its status and close cleanly.
- Require structured artifact identity, loader, lifecycle and cleanup telemetry.

### 3. Make prefixes persistent and isolated

- Provide `drive_c`, environment, registry views, Windows directories and
  crash-safe storage.
- Complete DOS-device/NT path normalization, sharing and directory enumeration.
- Expand thread, event, mutex, semaphore and wait semantics needed by startup.
- Prove state survives relaunch and that two application prefixes do not leak
  registry or filesystem state into each other.

### 4. Add portable-application import

- Define and validate the copy-and-run manifest and application namespace.
- Fail closed on missing architecture, EXE, runtime or prefix inputs.
- Run a second independent PE32 application so Pinball-specific behavior cannot
  become a runtime contract.

### 5. Migrate the first playable target

- Launch Pinball through Wine and the persistent prefix rather than the direct
  Win32 bootstrap.
- Preserve its existing graphics, audio, input and clean-close hardware gates.
- Produce a self-contained early package whose runtime contains no proprietary
  application files; the owner supplies those files during local packaging.

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

- Direct `setup.exe` execution without requiring a desktop shell, followed by
  MSI and multi-process installer support.
- An optional native library/launcher UI; Explorer compatibility is not needed
  for the core copy-and-run model.
- Store clients only after robust multi-process, IPC, networking, services and
  web/media support. They are not an early compatibility target.
- Broader multimedia, networking and filesystem compatibility.
- Wine-compatible exceptions, multi-threading and synchronization.
- PE64/x86-64 imports, callbacks, ABI/unwind support and an isolation policy.
- Additional graphics APIs only after the D3D9 backend is stable.

## Non-goals

- DRM or anti-cheat bypass.
- Kernel drivers.
- Shipping proprietary executables, DLLs, shaders, captures or SDK material.
- Claiming compatibility from static imports or a single screenshot.
