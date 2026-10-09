# D3D9 device query payloads

The getter schema names 28 exact IDirect3DDevice9 vtable slots. It covers scalar
state, display/raster status, transform/viewport/material/light/clip/scissor
structures, palettes, shader constant arrays and three value-return queries.
Objects returned by GetTexture/GetStreamSource and similar methods require a
separate registry-aware path and are not represented as pointers here.

A request is six little-endian DWORDs: version 1, method, argument 0, argument 1,
expected output bytes, reserved zero. Unused arguments must be zero. Constant
arrays use start register and element count; the count is multiplied with a
64-bit intermediate and bounded to 4096 output bytes before allocation/copying.
A zero count is preserved for the backend to judge. Scalar selector ranges and
semantic argument validity are likewise delegated to the actual D3D9 device.

A reply is version 1, method, exact HRESULT and output bytes, followed by exactly
that many bytes. The decoder checks the original request, including method and
expected successful length. Failure HRESULTs carry no output data. For UINT,
BOOL or float return methods, successful transport uses S_OK and the backend's
actual returned value bits. A failed transport must not be reported as a valid
fabricated value. BOOL output bits are preserved, including nonzero values
other than 1; float bits, including NaN encodings, are copied unchanged.

Numeric payloads are individually encoded little-endian DWORDs. Array elements
and named structures use native D3D9 field order, with no pointers or padding:
mode width/height/refresh/format; raster in-vblank/scanline; row-major matrix;
viewport x/y/width/height/minZ/maxZ; material diffuse/ambient/specular/emissive/
power; light type/diffuse/specular/ambient/position/direction/range/falloff/
attenuation0/1/2/theta/phi; clip union/intersection; rect left/top/right/bottom.
The palette is 256 explicit red/green/blue/flags byte entries.

Encoders and decoders leave output buffers, sizes and DTOs unchanged on invalid
input or insufficient capacity. Storage must not overlap inputs. Successful
replies are published only after exact framing and length checks. Native helper
and production guest COM integration must invoke this codec and copy outputs
only after successful HRESULTs. The codec itself neither calls the backend nor
maintains a shadow of its state.

The focused test exercises every schema, every truncated reply length, request
reserved fields, wrong-method replies, bounds/overflow, zero-length constants,
failed HRESULTs, native-valued bit patterns, palettes and atomic outputs. PE32
and PE64 builds also assert each named method's actual vtable offset.
