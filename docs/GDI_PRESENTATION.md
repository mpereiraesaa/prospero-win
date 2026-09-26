# Presenting GDI surfaces through ps5-vulkan WSI

Status: the route is implemented as the opt-in `PW_PRESENT_BACKEND=vk`
runtime backend and **validated on PS5 for the direct runner's GDI
surfaces** (Pinball); see [hardware validation](HARDWARE_VALIDATION.md). Wine
still has no presentation path, and GDI remains the bounded model described in
[GDI.md](GDI.md), not a complete GDI.

Hardware found one gap the host tests could not: ps5-vulkan's native queue
refused the `PRESENT_SRC`↔`TRANSFER_DST` barriers the recorder accepts
(`PS5VK_UPLOAD_PREPARE_FAILED rc=-8`), because the companion host test stubs
that native segment. ps5-vulkan PR #562 accepts exactly those two forms. The
title must also stay clear of the PE32 image's fixed base: the backend links
ps5-vulkan with `--gc-sections` and replaces its unused SPIR-V compiler with
fail-closed stubs (`native/pw_psbc_absent_ps5.c`), keeping the image near
0.7 MiB instead of 15.6 MiB.

## Verified starting point

Checked on 2026-09-26 against prospero-win `main` `ac0cac0` and ps5-vulkan
`main` `3efdf27`.

**prospero-win.** `pw_gdi` is a CPU model: target and bitmap surfaces live in
one caller-provided arena as 32-bit top-down B,G,R,A rows with
`stride = width * 4` (`surface_bytes`, `src/pw_gdi.c:39`). Bottom-up DIBs are
flipped when drawn (`pw_gdi_create_dib_bitmap`, `:159`; `pw_gdi_stretch_dibits`,
`:419`), never when presented. The only consumer is the direct PE32 runner:

```text
native/runtime_main.c  presentation_window()        focused/largest target >= 600x400
                       main loop, every 33.3 ms      FNV hash, skip unchanged frames
  -> pw_gdi_target_view()                            borrowed {pixels,w,h,stride,bytes}
  -> native/pw_videoout_ps5.c pw_videoout_ps5_present()
       CPU background fill + integer scale (1..3) + GFX1013 tiling into a scratch frame
  -> native/pw_agc_ps5.c pw_agc_ps5_copy_flip()
       WaitUntilSafeForRendering, DMA scratch -> scanout, SetFlip, RELEASE_MEM fence,
       submit + suspend point, bounded fence wait; then sceVideoOutWaitVblank
```

The Wine bootstrap (`native/wine_main.c`) accepts a `graphics=gdi` profile
(`:271`) but has **no presentation path at all**; the staged Wine runtime is
`ntdll`/`kernelbase`/`kernel32` only. Parallel, uncommitted work is wiring
`pw_gdi` to Wine's win32u `NtGdi*` calls and adds guest-backed DIB sections.
This proposal therefore depends only on the stable `pw_gdi_target_view` seam.

**ps5-vulkan.** The WSI route is real but bounded:

| Step | Code | Contract |
| --- | --- | --- |
| Surface | `vkCreateDisplayPlaneSurfaceKHR` | one 1920×1080 60 Hz display, identity transform |
| Swapchain | `src/vk_swapchain.c` `vkCreateSwapchainKHR` | exactly 2 images, `B8G8R8A8_UNORM`, sRGB-nonlinear, 1920×1080, `OPTIMAL` tiling, usage ⊆ `COLOR_ATTACHMENT`\|`TRANSFER_DST`, FIFO, opaque alpha, no `oldSwapchain`; one swapchain per device and surface |
| Scanout | `native/wsi_present_ps5.c` → `native/present_ps5.c` | the two images **are** the VideoOut buffers; ps5vk opens VideoOut itself |
| Acquire | `vkAcquireNextImageKHR` | returns an image that is neither acquired nor `display_busy` |
| Upload | `src/vk_transfer.c` `vkCmdCopyBufferToImage` (`:125`) | tiled BGRA8 target requires `TRANSFER_DST_OPTIMAL`; honours `bufferRowLength` and `imageOffset` |
| Execute | `src/vk_image_transfer.c` (`:1400`) | copy runs on the **CPU** in the queue executor, texel by texel through the measured 64KB_R_X equation, then flushes the image |
| Barriers | `src/vk_command.c` `image_barrier_profile` (`:1450`) | accepts `PRESENT_SRC→TRANSFER_DST` (0 → `TRANSFER_WRITE`) and `TRANSFER_DST→PRESENT_SRC` (`TRANSFER_WRITE` → 0/`MEMORY_READ`); these are **native GPU segments** that commit layouts in `native/graphics_queue_ps5.c` |
| Present | `vkQueuePresentKHR` | drains the queue (3 s bound), requires `PRESENT_SRC`, then flips synchronously: success means fence plus matching flip event (`src/wsi_present_backend.h`) |
| Memory | `API.md:746` | one device-local, host-visible, **non-coherent** type: writers must `vkFlushMappedMemoryRanges` |

Hardware evidence exists for the pieces separately, not together: PR #520
presented three frames through the swapchain using render-pass clears, and
PR #517 validated buffer→tiled-BGRA8 copies into ordinary images. No run has
copied CPU pixels into a swapchain image and presented it.

## 1. Can WSI present `pw_gdi` pixels directly?

**No. One copy into driver-owned memory is required, and one composition step
is required unless the window already is 1920×1080.**

| Property | `pw_gdi` target | Swapchain image | Consequence |
| --- | --- | --- | --- |
| Byte order | B,G,R,A | `B8G8R8A8_UNORM` | identical; no channel swizzle |
| Alpha | 0 after `BLACKNESS`/zero-fill | opaque composite | force `0xff`, as the AGC path already does |
| Orientation | top-down | top-down | no flip |
| Row stride | `width*4`, arbitrary width | tiled | `bufferRowLength = stride/4` |
| Extent | any (e.g. 600×416) | fixed 1920×1080 | placement, optional scale, background |
| Memory | caller arena, may move on resize | driver allocation pinned to VideoOut | no import path (no external-memory-host) → copy |
| Tiling | linear | 64KB_R_X | ps5vk's executor performs linear → tiled |

A background clear with `vkCmdClearColorImage` is native GPU work on this
target, and blits into it are outside the validated transfer role. The simplest
truthful route is therefore: compose on CPU into a linear 1920×1080 staging
buffer, then one whole-image `vkCmdCopyBufferToImage`. Per frame this is two
CPU passes (compose + ps5vk tiling); today's path is also two passes (CPU tiled
compose + GPU DMA). Their relative cost is unmeasured.

## 2. Contracts to resolve

- **Format and stride.** `PW_PRESENT_BGRX8`: 4 bytes per pixel, stride a
  multiple of 4 and ≥ `width*4`. Alpha is ignored on input and written opaque.
  sRGB-nonlinear UNORM means values pass through unchanged, as on the AGC path.
- **Orientation.** Frames are top-down; any bottom-up DIB is resolved inside
  GDI. Neither backend flips.
- **Extent.** The display extent is fixed per backend. `pw_present_fit`
  refuses a frame larger than the output (`PW_ERR_LIMIT`) before any image is
  acquired. The AGC presenter crops instead (`native/pw_videoout_layout.h`):
  an axis that fits stays centred and an oversized axis shows its leading
  part at scale 1. It previously computed `left=(WIDTH-shown_w)/2` without a
  guard, so a target wider than 1920 or taller than 1080 underflowed and wrote
  outside the scratch frame.
- **Ownership and lifetime.** The frame is borrowed from GDI and valid only
  until the next resize, destroy or reset of that target
  (`pw_gdi_resize_target` can move the offset, `src/pw_gdi.c:301`). It must be
  consumed synchronously on the guest thread. Staging memory belongs to the
  backend. Swapchain images stay pinned from creation to destroy; the displayed
  image is not writable until the next flip retires it. Guest-backed DIB
  sections must not become target surfaces without a copy-at-safepoint rule.
- **Synchronization.** Single producer (the guest thread). Order per frame:
  compose → flush → acquire (fence) → record barrier/copy/barrier → submit with
  fence → wait → present. Present already drains and returns only after the
  flip event, so there is at most one frame in flight and FIFO pacing is
  implicit. The existing 30 Hz cadence and unchanged-hash skip stay in the
  runner, above the seam.
- **Presentation ownership.** VideoOut is single-owner. `pw_videoout_ps5` and
  a ps5vk swapchain both open it, so a process selects exactly one backend.
  ps5vk also permits one swapchain per device and surface: when DXVK arrives,
  GDI windows and Direct3D swapchains must go through one compositor that owns
  the only swapchain. Teardown order is swapchain → device → surface, and
  `lifetime_errors` must stay zero.

## 3. Minimal reusable interface

[`src/pw_present.h`](../src/pw_present.h) is portable core code (no Vulkan,
AGC or VideoOut types):

```c
PwPresentFrame  {pixels, width, height, stride, format}   /* borrowed, top-down */
PwPresentSink   {context, width, height,
                 acquire(context, PwPresentTarget *),      /* backend lends linear memory */
                 submit(context, sequence, commit)}        /* synchronous copy-out + flip */
pw_present_frame_from_gdi(view, frame)
pw_present_fit / pw_present_compose
pw_present_frame(sink, frame, margin, max_scale, background, sequence, placement)
```

Every refusal happens before `acquire`; after a successful `acquire` exactly
one `submit` follows, with `commit=0` to abandon. The runner and a future Wine
display path produce a `PwPresentFrame`; only a native backend knows Vulkan.

Backend mapping (the Vulkan WSI backend is implemented in `native/pw_present_vk_ps5.c`; the default AGC path does not use the sink yet):

- **AGC backend** (`native/`, wraps today's code): `acquire` lends a linear
  scratch; `submit` tiles it and calls `pw_agc_ps5_copy_flip`.
- **Vulkan WSI backend** (`native/pw_present_vk_ps5.c`, opt-in, links the
  ps5vk SDK): at open, create instance with `VK_KHR_surface`+`VK_KHR_display`,
  display-plane surface, device with `VK_KHR_swapchain`, a transfer-only
  swapchain and a persistently mapped 8 MiB `TRANSFER_SRC` staging buffer.
  `acquire` returns the mapped staging buffer. `submit(commit=1)` flushes it,
  acquires an image with a fence, records the two barriers around one
  whole-image copy, submits and waits, then presents. `submit(commit=0)` does
  nothing because no image has been acquired yet.

Under Wine the producer changes, not the seam: whether frames come from
`pw_gdi` behind win32u `NtGdi*` calls or from a Wine display driver's window
surface, a Unix-call handler exposes 32-bit top-down BGRX bits as a
`PwPresentFrame`.

## 4. Local proof and console evidence

Local, host only:

- `tests/test_pw_present.c` (this repository): real `pw_gdi` drawing (bottom-up
  32-bit `StretchDIBits`, `BLACKNESS`) → `pw_gdi_target_view` → frame → fit →
  compose into a padded target → recording sink. It asserts exact pixels,
  upright orientation, opaque alpha, placement, backend-owned padding, the
  copy-out ownership rule and every pre-acquire refusal.
- ps5-vulkan `tests/test_wsi_buffer_present.c` (companion change): through the
  public API, creates a transfer-only swapchain, flushes a non-coherent staging
  buffer, acquires, records `PRESENT_SRC→TRANSFER_DST`, copy,
  `TRANSFER_DST→PRESENT_SRC`, submits, waits and presents twice. It detiles
  both images and compares every one of 1920×1080 texels, including a
  600×416 region with `bufferRowLength=608` at an offset. The display and the
  native barrier segment are stand-ins, so this is not GPU evidence.

Before calling the route validated on PS5:

1. Exact ELF/fSELF hashes, the ps5vk SDK revision, clean deployment and fresh
   mount.
2. `PW_RUNTIME_READY` naming `present_backend=vk-wsi`, swapchain extent,
   image count and usage.
3. `PW_VIDEO_FRAME` with alternating slots, strictly increasing tokens,
   fence-complete and matching-flip evidence for every frame, and a staging
   hash that equals the GDI-composed hash.
4. A deterministic pixel oracle: a test build that detiles the presented
   image after the copy and matches the staging hash.
5. Changing frames over a continuous run with no abort, signal, device loss or
   lifetime error; orderly teardown (swapchain, device, surface, VideoOut) and a
   gap-free `BYE`, with `tools/validate_runtime_evidence.py` extended for the
   new backend.
6. Remote Play capture only as supporting evidence.

## 5. Launcher

`WINE_INTEGRATION.md` describes a launcher that selects a profile's EXE. Its
model (`pw_launcher_model`, `pw_prefix_registry`) exists only on the unmerged
local branch `codex/prefix-launcher-foundation`, with no renderer, so no UI
code is added here. The proposed order:

1. Integrate the launcher model and its tests.
2. Add a portable `pw_launcher_render` that draws the catalogue into a
   `PwPresentFrame` (fixed bitmap font, selection highlight), host-tested by
   exact pixels.
3. Run the launcher and the guest through the **same** `PwPresentSink`
   instance, since VideoOut and the swapchain are single-owner.

With the Vulkan backend the launcher then uses WSI for presentation. Drawing
the UI with Vulkan itself (render passes, pipelines, sampled glyph textures) is
possible in ps5-vulkan's bounded profile but adds shader and texture
dependencies without benefit for a text catalogue; software composition is the
first step.
