# Typed device object getters

The frontend installs nine exact IDirect3DDevice9 slots: GetBackBuffer, GetRenderTarget, GetDepthStencilSurface, GetTexture, GetVertexDeclaration, GetVertexShader, GetStreamSource, GetIndices and GetPixelShader. It validates required outputs, method-specific reply shape and exact HRESULT before publishing results. Failed calls leave caller outputs unchanged; successful null bindings remain null. Stream offset and stride publish together with the wrapped buffer.

The getter callback returns one owned remote reference. The wrap callback consumes that reference on every path, including allocation failure. The failure callback cancels the session so its service retires malformed publications. A strong device reference spans getter, validation and wrapping, including reentrant release of the caller's last reference. Install callbacks once before publishing the vtable. Registry and session integration remain separate.

## Validation

`/tmp/prospero-d3d9-device-object-methods-r1/receipt.json` records successful actual PE32 and PE64 controlled-callback fixtures. Both exercise all nine slots, exact positive HRESULT, null bindings, stream metadata, required outputs, malformed replies, wrapping failure and reentrant last-reference release. This proves the typed frontend and ownership boundary; it does not claim native session or console coverage.

```sh
python3 tests/lab/d3d9_device_object_methods.py --wine-build /tmp/prospero-bridge-native-domain/build --prefix /tmp/prospero-d3d9-device-methods-prefix --output /tmp/prospero-d3d9-device-object-methods-r1
```
