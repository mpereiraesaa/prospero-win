# D3D9 object-returning device queries

Nine device getters return opaque registry identities: back buffer, render
target, depth/stencil surface, texture, vertex declaration, vertex shader,
stream source, index buffer and pixel shader. The schema pins their exact COM
slots and expected resource kinds. The first texture path represents 2D textures;
cube and volume textures need explicit resource kinds and proxy support.

Requests contain six little-endian DWORDs: version 1, method, three argument
words and reserved zero. GetBackBuffer uses swap-chain index, back-buffer index
and type. GetRenderTarget, GetTexture and GetStreamSource use one index. Other
methods have no arguments. All unused words must be zero.

Replies contain eight DWORDs: version 1, method, exact HRESULT, kind, object ID,
generation, offset and stride. Offset/stride are actual GetStreamSource outputs;
other methods require zero in those fields. Null bindings are represented by
zero kind/ID/generation. Non-null bindings require all three and the method's
expected kind. Failed HRESULTs contain no output fields. Reply decoding checks
the original request before publishing any output.

The native dispatcher must validate the outer session/device, call the real
backend, and retain the returned COM reference until registry publication.
It must reuse a live canonical IUnknown identity with a guest reference increment
or create a correctly typed wrapper and retain its parent device/window context.
Publication failure releases the backend reference and returns a failure. An
already-known identity must not acquire a second independent guest proxy.
Null bindings must remain null. No native pointer or handle appears on the wire.

The codec provides framing and identity contracts only. Native querying,
canonical registry lookup, returned-reference ownership and guest proxy lookup
are mandatory integration work. Its methods do not bypass lifetime validation
or supply synthetic default objects. Inputs and outputs must not overlap;
invalid input and short output storage leave DTOs/buffers/sizes unchanged.

The focused test covers all nine schemas, PE32/PE64 vtable positions, every
truncated length, wrong kinds/methods, partial null identities, stream outputs,
failed HRESULTs and atomic output publication.
