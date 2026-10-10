# Where a frame's time goes on DXVK's CS thread

A 32-bit Direct3D game on the native CPU runs DXVK's command-stream (CS)
thread in three places: DXVK's own code (32-bit guest code), RADV (the
Vulkan driver, native code reached through Unix calls) and Wine's syscalls.
The HUD's `CS load` says how busy that thread is; the per-thread profile
(`PW_NATIVE_PROFILE`, [wine/wow64native/README.md](../wine/wow64native/README.md))
says which of the three over two-second windows. Neither says what happens
in the frames that take 25 ms instead of 16. This profile does, per frame:

| Line | From | Says |
| --- | --- | --- |
| `PW_VK_RADV_PRESENT` | `libvulkan.prx` | each present: its number, the TSC, the ticks since the previous present (the frame's length), the call's own ticks and its result |
| `PW_NATIVE_PROFILE version=3` | `wow64native.prx` | each native thread, each frame: ticks in Unix calls, syscalls, other host entries and FS switches since its previous line; the rest of `wall` is guest code |
| `PW_VK_RADV_PROFILE` | `libvulkan.prx` | each thread that called the driver, each frame: calls and ticks by entry point, the twelve with the most time named and the rest as `other` |
| `PW_VK_RADV_EVENT` | `libvulkan.prx` | one call that took longer than the event threshold (200 µs unless `PW_VK_RADV_EVENT_US` says otherwise), with its arguments when it creates, allocates or waits: pipeline flags, allocation size, image extent, timeouts |
| `PW_NATIVE_SLOW_SYSCALL` | `wow64.dll` (`PW_NATIVE_SLOW_SYSCALL_US=<n>`, [native-system-service-profile.md](native-system-service-profile.md)) | one guest system service that took longer than n µs: its name, arguments, status and the guest stack by module, for the waits the buckets only count |
| `DXVK_CS_PROFILE` | DXVK 2.6.2-prospero2 (`DXVK_CS_PROFILE=1`) | the CS thread, each frame: its busy ticks, chunks and commands, and ticks by command type, named by the D3D9 call that emitted it |

All ticks are TSC ticks; `tsc_hz` is in the `PW_NATIVE_PROFILE` lines and in
the driver's `PW_VK_RADV_PROFILE version=1 start` line. The `env` line after
it gives the shader-cache variables Mesa reads (`MESA_SHADER_CACHE_DIR`,
`XDG_CACHE_HOME`, `HOME`, …), the root it would derive its cache directory
from and whether the process can write there: without a writable cache every
run compiles every pipeline again, which is what a slow first minute looks
like. A frame is a present:
the driver's count, which the DXVK line reaches through its own frame id.
Every line stays under a ps5log record.

## How the lines are made

RADV's loader shim (`wine/ps5/pw_vulkan_radv.c`) hands Wine every entry point.
With the profile on it hands out a timing wrapper instead
(`wine/ps5/pw_vk_radv_profile_wrap.c`, one per function in
`pw_vk_radv_profile_list.h`: the device functions DXVK 2.6.2 calls). A wrapper
reads the TSC, calls the driver, reads it again and adds the difference to the
calling thread's table. `vkQueuePresentKHR`'s wrapper also moves the present
count. On its first call after that, each thread writes its table for the
frames that ended, clears it and calls the frame hook, which `wow64native.prx`
installed: that writes the thread's host/guest split for the same interval.
A thread that does not call the driver every frame writes a line with
`frames` greater than 1, which the tool leaves out of per-frame means.

The profile is on when `PW_VK_RADV_PROFILE=1` or `PW_NATIVE_PROFILE=1` is in
the game's environment when the driver resolves its entry points, or when the
hook is installed before then. With it off, the shim returns the driver's own
pointers and nothing is timed.

## Running one

In the game's profile:

    [debug]
    env = PW_NATIVE_PROFILE=1
    env = DXVK_CS_PROFILE=1

`PW_NATIVE_PROFILE=1` turns on both the per-thread profile and, through the
hook, the driver's. `DXVK_CS_PROFILE=1` needs the 2.6.2-prospero2 `d3d9.dll`
in the prefix. Play three to four minutes of the scene that dips, moving all
the time (standing still hides the drops), with the frame rate capped as the
owner plays (60 Hz). The log server's copy of the session is the one to read:
the console's own ring loses the middle of a long run.

The lines cost the CS thread a few microseconds a frame (one `fprintf` per
line, two reads of the TSC per driver call) and the event lines nothing unless
a call was slow. Compare a profile-off run before attributing a frame-rate
change to anything else.

## Reading one

    tools/native_profile_frames.py LOG [--spike-ms 25] [--normal-ms 20] [--tid 004c] [--csv frames.csv]

prints:

1. the frame-length distribution (p50, p90, p99, max, and how many frames
   passed 25, 33 and 50 ms), the counts of normal and spike frames and the
   worst ten;
2. for the CS thread (the native thread with the most Unix calls per frame,
   or `--tid`): the mean milliseconds per frame in guest code, Unix calls,
   syscalls, other and FS switches, in normal frames and in spikes, with the
   difference: which bucket the spike is in;
3. the driver entry points on that thread, milliseconds per frame in normal
   frames and spikes, ordered by their time in spikes, and the total: which
   calls the extra driver time is in, and how many of them;
4. the other threads that call the driver (DXVK's submit and compiler
   threads), with their busiest entry points;
5. the slow calls: how many, in which functions, how many fell in spike
   frames, and the slowest with their arguments: a pipeline compiled at the
   wrong moment, an allocation, a wait that returned late;
6. with the DXVK build, the CS thread's busy time and the command types that
   grow most in spikes.

`--csv` writes one row per frame (length, the buckets, driver time, DXVK busy
time) for a chart. A best case for a candidate fix reads straight off the
table: a function's spike-frame milliseconds are what removing it would save
in such a frame.

What it does not say: which function inside the guest code or inside a driver
entry point took the time. A sampling profiler would, but signal-based
sampling is unsafe on the console (the debugging guide, section 8), so this
profile stops at the command and entry-point level, where a fix can usually
be chosen anyway.
