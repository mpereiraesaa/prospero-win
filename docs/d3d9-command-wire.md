# Synchronous D3D9 state and draw payloads

This codec defines40 device methods with HRESULT returns and no output data.
Each method requires an ordered synchronous service call and its exact backend
HRESULT. It provides no asynchronous-success shortcut, local state shadow or
backend dispatch. The method inventory stays unsupported until a complete
adapter supplies validation, object retention, execution and error handling.

Request header: version1, device vtable slot, argument word count, data byte
count (four little-endian u32 words). Declared argument words follow, then the
owned data. Exact length is required. Reply: version1, method, HRESULT bits,
reserved0 (16 bytes). The adapter compares method and HRESULT with the outer
request/reply correlation before exposing results.

The scalar arguments appear in native parameter order with pointer parameters
removed, except that an object argument is replaced by two words: ID then
generation. A null object is exactly0/0. Both words must otherwise be nonzero;
the service verifies type, device, epoch and live/queued lifetime before use.
No native pointer, handle or struct layout belongs in this payload.

| Methods | Argument words | Owned data |
| --- | --- | --- |
| TestCooperativeLevel, EvictManagedResources, BeginScene, EndScene | none | none |
| SetDialogBoxMode, SetSoftwareVertexProcessing | BOOL0/1 | none |
| SetRenderTarget, SetTexture | index/stage, object ID, generation | none |
| SetDepthStencilSurface, SetVertexDeclaration, SetVertexShader, SetIndices, SetPixelShader | object ID, generation | none |
| Clear | rectangle count, flags, color, depth float bits, stencil | count rectangles, each x1/y1/x2/y2 signed bits |
| SetTransform, MultiplyTransform | transform state |16 matrix float words in row order |
| SetViewport | none | X/Y/Width/Height u32, MinZ/MaxZ float bits |
| SetMaterial | none | Diffuse/Ambient/Specular/Emissive RGBA floats, Power float |
| SetLight | index | type u32; Diffuse/Specular/Ambient RGBA; Position XYZ; Direction XYZ; Range/Falloff/Attenuation0/1/2/Theta/Phi float bits |
| LightEnable | index, BOOL0/1 | none |
| SetClipPlane | index |4 float words |
| SetRenderState | state, value | none |
| SetClipStatus | none | ClipUnion/ClipIntersection u32 |
| SetTextureStageState, SetSamplerState | stage/sampler, type, value | none |
| SetPaletteEntries | palette index |256 ordered red/green/blue/flags byte entries |
| SetCurrentTexturePalette, SetFVF | value | none |
| SetScissorRect | none | left/top/right/bottom signed bits |
| SetNPatchMode | segment float bits | none |
| DrawPrimitive | type, start vertex, primitive count | none |
| DrawIndexedPrimitive | type, signed base vertex bits, minimum vertex, vertex count, start index, primitive count | none |
| SetVertexShaderConstantF/I, SetPixelShaderConstantF/I | start register, vector count |4 scalar words per vector |
| SetVertexShaderConstantB, SetPixelShaderConstantB | start register, count | count BOOL0/1 words |
| SetStreamSource | stream, buffer ID, generation, byte offset, stride | none |
| SetStreamSourceFreq | stream, setting | none |

Except palette entries, data is represented locally as scalar u32 words and
encoded individually as little endian; signed integers and IEEE754 values retain
their bits. Adapters map each declared field, normalize BOOLs, copy caller arrays
before publication, and retain referenced objects until completion. Palette data
is an explicit sequence of bytes. Formats and ordinary enum values remain for
backend validation.

Storage is bounded to4096 data bytes, sufficient for256 float/int vectors or256
clear rectangles. Overflow and larger inputs return codec UNSUPPORTED before
copying; adapters must not truncate or report backend success. A future upload
transport can extend coverage explicitly. Encoders and decoders publish only on
success; caller input/output storage must not overlap.

UP draws are deliberately absent. The pinned [DXVK implementation](https://github.com/doitsujin/dxvk/blob/9d6f54a1ade20d1d27dd421024717a636f3d8c68/src/d3d9/d3d9_device.cpp)
derives upload size from vertex declaration state; indexed uploads also include
the minimum vertex prefix. Guessing primitive count times stride would not prove
a safe complete copy. Resource creation, getters, query results, state blocks
and UP uploads require their own reviewed adapters.

## Encoder temporary initialization

The encoder retains its local temporary to preserve overlapping command/output
buffers, but does not clear the entire 4,144-byte capacity before each call.
The published range has no gaps: four header words fill bytes 0..15, the schema
argument words immediately follow, then exactly the validated payload. Every
payload shape is a multiple of four bytes; scalar-word payloads use explicit
little-endian stores and palette payloads use an exact byte copy. Only that
fully written range is copied to the destination. Unused temporary bytes are
never published.

All validation and error precedence precede destination publication. The
existing `written` result behavior is unchanged, including required length on
insufficient capacity. The final temporary-to-output copy retains alias safety.
A frozen initialized-encoder reference checks every method, each output capacity,
maximal vector/BOOL/clear payloads, zero payloads, opaque bits, poisoned unused
fields, invalid inputs and overlapping source/output positions. This is a
source-level initialization reduction, not a measured runtime speedup.
