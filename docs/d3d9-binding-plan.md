# Binding admission plan (inactive)

This portable helper describes six canonical binding DTOs. It does not enable
asynchronous acceptance: `pw_d3d9_command_can_queue` still rejects every object
word, including null bindings. READY requires separate typed same-device/session/
epoch/generation authorization and a lifetime ticket before queue admission.

The source contract is DXVK `5fde742bd8fc0c3e9f062caa91ef0e27786df1b4`,
`src/d3d9/d3d9_device.cpp` and `d3d9_stateblock.cpp`:

| Method/slot | Scalar contract | Required kind |
|---|---|---|
| SetTexture/65 | samplers 0–15, 256–260 | Texture2D/5 |
| SetVertexDeclaration/87 | no scalar rejection | declaration/7 |
| SetVertexShader/92 | no scalar rejection | vertex shader/8 |
| SetStreamSource/100 | stream below 16 | vertex buffer/3 |
| SetIndices/104 | no scalar rejection | index buffer/4 |
| SetPixelShader/107 | no scalar rejection | pixel shader/9 |

Valid typed objects and canonical null `{0,0}` are accepted by these native
methods with S_OK, including stateblock recording. Stream offset/stride are not
rejected here by the pinned backend. Out-of-range streams return INVALIDCALL.
Invalid texture samplers are native S_OK no-ops, but remain on the existing
synchronous path because the current bridge resolves objects before native
invocation. This helper does not repair or bypass that ordering. Cube/volume
textures are outside the current typed Texture2D contract.

Null stream/index updates have backend-specific hardware-binding behavior;
execute the actual methods rather than manufacturing an unbound shadow state.

Future admission must retain private queued object references through consumption
or cancellation. Public AddRef is unsuitable: it can hide public Release0 and
suppress the flush needed to consume an unpublished partial batch. A queued cache
hold must preserve public counts, trigger Release0 draining outside cache locks,
and finalize after acknowledgement/cancel. Service preparation must pin referenced
registry generations before execution; this alone does not protect the earlier
client-admission interval. Release, Reset, stateblocks, getters and Present retain
ordered barriers. Neither guest callbacks nor RPC may run under cache locks.

The host fixture checks all six methods, null/nonnull IDs, canonical fields,
stream/sampler boundaries, unchanged failed outputs and continued policy rejection.
It does not prove typed object lifetime, native driver behavior, session ordering,
or console performance. Those are separate prerequisites for activation.
