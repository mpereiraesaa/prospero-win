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

This first wave is not the final performance scope. Draws81/82 require correct
live declaration state: absent declarations fail even with zero primitives;
SetFVF(0) does not clear a declaration, and state-block recording/application and
Reset change the required shadow. Constants require exact software-register
bounds, overflow checks and hardware-layout clamping before NULL testing.
Object bindings additionally require typed generation pins until execution or
cancellation. These remain synchronous until their respective proofs exist.

The portable fixture checks copied values, malformed lengths, unused arguments,
all sampler boundaries, texture-stage enum holes and all other method exclusions.
Actual native parity is a separate prerequisite before runtime enablement.
