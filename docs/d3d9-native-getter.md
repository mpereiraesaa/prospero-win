# Native D3D9 getter adapter

`pw_d3d9_native_getter_dispatch` maps the 28 bounded getter codec methods to
an actual native `IDirect3DDevice9`. Its caller owns the device reference and
executes on the native owner thread. Production session transport is separate;
GETTER_CALL uses opcode 24.

The return value is codec status. A valid dispatched call returns codec success
and preserves the backend HRESULT in the reply, including failure HRESULTs.
Failures publish zero output bytes. Unsupported or malformed requests leave the
reply untouched and do not call the backend. The four value-return methods
publish S_OK and the exact backend UINT, BOOL, or float bits. Native structures
are reconstructed field by field; float bits and noncanonical BOOL values survive.

Shader constant getters always receive valid native temporary storage, including
count zero. The guest proxy validates required output pointers before encoding.
The wire count remains unchanged. No native pointer enters the wire payload.

## Focused proof

Run `tests/lab/d3d9_native_getter.py` with `--wine-source`, `--wine-build`,
`--prefix`, `--backend64`, and a new `--output` directory. The runner builds a
controlled native-vtable fixture using Wine headers, repeats with ASan/UBSan,
and builds actual PE32 client and PE64 service DLLs. Three independent processes
use the private native bootstrap and a real DXVK device.

The controlled fixture covers every method, distinct HRESULTs, atomic backend
failures, exact float/BOOL bits, maximum arrays, zero arrays, and rejected malformed
requests. Each real process sends 32 requests: all 28 methods, three zero arrays,
and a nonexistent light. State written to the real device is checked on readback.
This DXVK backend rejects palette getters even though palette setters succeed;
the fixture compares direct backend and helper HRESULTs and preserves failure.

Receipt: `/tmp/prospero-d3d9-native-getter-r2/receipt.json`.
This proves the native helper and real PE codec boundary. It does not exercise
production COM proxy/session wiring, a console, or game performance.
