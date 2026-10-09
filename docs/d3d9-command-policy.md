# First command queue policy

This portable predicate is tied to DXVK commit
`5fde742bd8fc0c3e9f062caa91ef0e27786df1b4`. It selects an initial immediate-S_OK
subset; it does not enable batching or promise successful queue admission.
Zero means use the synchronous path, not return an invented API error.

| Slot | Method | Eligible input |
| --- | --- | --- |
| 57 | SetRenderState | Any state/value DWORD; unknown indices remain backend no-ops |
| 67 | SetTextureStageState | Defined types1..11,22..24,26..28,32; any stage preserves backend clamp |
| 69 | SetSamplerState | Samplers0..15,256..260 and types1..13 |
| 47 | SetViewport | Complete owned24-byte value |
| 49 | SetMaterial | Complete owned68-byte value |
| 75 | SetScissorRect | Complete owned16-byte value |

Exact schema length and zero unused argument words are required. The predicate
reads an owned command, never original guest pointers. Pointer validation and
copying occur before this predicate; null or failed copies retain the established
synchronous behavior. Field values are preserved bit-for-bit without adding
validation that the pinned backend does not perform. Unusual enum values outside
the subset remain synchronous, even if some happen to return S_OK.

## Source contract

Pinned `d3d9_device.cpp` methods at2045/2076/2240/2752/2782/2829 and helpers
4370/4486 return S_OK for this subset; `d3d9_stateblock.cpp` recording methods
92/101/178/235/251/259 do likewise. `d3d9_util.h`52/75 defines sampler validation
and mapping;351 remaps texture-stage types before clamping. NULL material and
scissor pointers reject before recording, and the bridge must not turn failed
input copies into accepted records. These are exact-source claims; no console
performance or queued runtime acceptance follows from the portable test.

Do not reorder or coalesce commands: render-state RESZ can trigger resource work.
Ordered state-block boundaries must preserve live versus recorded state. Every
synchronous getter, Reset, resource operation and Present observes preceding
accepted commands. Admission checks negotiated backend policy, session health,
space and device lifetime before returning S_OK. Any unexpected non-S_OK backend
result sticks failure, records its sequence and wakes waiters. The service must
independently validate all commands before executing any batch member.

This is not the final performance scope. Draws81/82 require correct
live declaration state: absent declarations fail even with zero primitives;
SetFVF(0) does not clear a declaration, and state-block recording/application and
Reset change the required shadow. Object bindings additionally require typed generation pins until execution or
cancellation. Draws and object bindings remain synchronous until their respective proofs exist.

The portable fixture checks copied values, malformed lengths, unused arguments,
all sampler boundaries, texture-stage enum holes and all other method exclusions.
Actual native parity is a separate prerequisite before runtime enablement.

## Native comparison fixture

The existing native-command lab additionally compares596 direct backend/helper
calls per process: all six families, every accepted sampler slot/type, all
accepted texture-stage types with stage-clamping edges, unknown render-state
no-op, and conservative invalid-sampler fallback. Each case runs both live and
inside state-block recording. NULL material/scissor rejection is checked directly.
All original command/draw/readback checks remain. The runner freezes local
headers and implementation inputs. This is native helper parity, not a queued
session test. Fresh retained host proof passed all596 comparisons in each of
three native processes, with successful draws/readback/Present and clean exit.
The receipt freezes89 implementation/header inputs and9 successful commands.
Portable policy normal and ASan/UBSan tests separately passed4 commands.
These results do not establish queued session acceptance or performance.

## Constants and transforms

The policy also admits SetTransform44 for VIEW2, PROJECTION3, TEXTURE16..23
and WORLD256..511 with an owned64-byte matrix. Other indices remain synchronous.
Pinned `d3d9_util.h`183 maps these into266 entries; device helper4469 and the
state-block recorder both copy matrices and return S_OK.

Constant methods94/96/98/109/111/113 require exact owned bytes, canonical BOOL
values and checked software-register bounds. The normalizer preserves backend
validation order: reject addition overflow and software-range excess, clamp to
the creation-time layout, then reject NULL only if effective count is nonzero.
VS limits are8192 float/2048 integer or BOOL software registers; hardware-only
creation copies at most256/16. Mixed or software creation uses the extended
layout regardless of later SetSoftwareVertexProcessing. PS limits are224/16.
Source: device.cpp7775,4639; device.h1269,1128; d3d9_caps.h14..18.

The policy itself sees already owned data and validates software bounds; the
frontend normalizer must run before reading guest data, using immutable local
creation flags. Getters and state-block/Reset calls still drain prior commands,
so no local constant-value shadow is claimed. Large software-mode payloads over
the4096-byte transport capacity remain an explicit unsupported frontend limit;
this change must not allocate out of bounds, split a logical call without
serialization, or invent successful execution for those uploads.

Expanded admission is policy revision2 and requires the additional HELLO feature
16384 on both peers, alongside command batching. An old first-wave-only peer
must not accept an expanded-policy pair. This foundation exports the required
feature and the production session adds it atomically to batch-enabled HELLO.
The exact feature equality check rejects first-wave/expanded mixed pairs before
object creation. Expanded production pairs advertise30719; first-wave pairs
remain14335. Runtime enablement still requires the existing explicit opt-in.

Retained host proof for this extension passed620 setter/Transform comparisons
and132 constant boundary comparisons per native process, across hardware, mixed
and software creation modes. Live and state-block recording paths both run.
The native receipt freezes91 inputs and9 successful commands; frontend staged
copy has a separate90-input,8-command normal/sanitized/PE32/PE64 proof. No
queued-session or console acceptance is claimed by these family proofs.
