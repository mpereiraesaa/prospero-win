# Native D3D9 state and draw adapter

`pw_d3d9_native_command_dispatch` implements the40 HRESULT-returning methods in
the bounded command codec. Its input is an already decoded, owned command and a
live native `IDirect3DDevice9` held by the service for the duration of the call.
The helper validates method shape, exact data length, scalar BOOLs, object token
pairs and BOOL arrays before native dispatch. It maps every scalar/structure
field explicitly; wire bytes are never cast to a native D3D structure.

Recognized valid calls return the exact backend HRESULT. Invalid local command
shape returns `D3DERR_INVALIDCALL`; a method outside the reviewed codec returns
`E_NOTIMPL`. The session must correlate and encode the returned HRESULT using the
command reply codec. There is no asynchronous success return or state shadow.

## Resource ownership

The acquisition callback receives ID, generation, expected kind and the target
native device. The service must validate kind/device/epoch/generation and obtain
one owned reference atomically under its registry lock, then release the lock
before returning. The pointer is the exact typed native interface, not a raw
wire pointer or an unrelated canonical identity interface. Failure returns no
reference. The helper releases the acquired reference after the backend call,
including backend failure. A null0/0 binding bypasses acquisition; the backend
retains bound resources according to its COM contract.

Kinds come from the shared resource header: surface6, texture2D5, vertex buffer3,
index buffer4, vertex declaration7, vertex shader8 and pixel shader9. Cube and
volume textures need separately reviewed kinds. Shader/declaration production
creation and registry wiring are separate adapters. Device lifetime and queued
request ownership remain the session's responsibility. This helper does not
access guest memory or call a guest callback.

## Evidence and limits

Run `tests/lab/d3d9_native_command.py` with the configured native-domain Wine
build/prefix, matching Wine source and native DXVK backend. The runner refuses
source changes during its proof and records command lines, results and hashes.
Its controlled native COM fixture checks all40 methods, exact field and
signed/float-bit mapping, arbitrary backend failure HRESULTs, eight typed
acquisitions/releases, failed resolution, null bindings, malformed inputs and
maximum bounded rectangle/vector/BOOL arrays. Host and ASan/UBSan variants run.

The real PE fixture creates payloads in PE32-owned shared memory and decodes them
inside the native PE64 service bootstrap. Three separate processes each exercise
all40 methods and48 total calls on a real DXVK device. Unbound draws return
`D3DERR_INVALIDCALL`; the subsequent valid primitive/indexed draws succeed, a
render-target readback differs from the clear color, and Present succeeds.
The exact payloads contain only scalar data and object tokens. Native test
resources are registered by a controlled resolver.

Receipt: `/tmp/prospero-d3d9-native-command-r2/receipt.json`.
This proves the codec/helper and native ABI. It does not claim production COM
proxy/COMMAND_CALL21 routing, complete D3D9 coverage, hardware console rendering,
or a performance improvement. Session integration and console validation remain
separate gates. UP draws, getters, state blocks and unreviewed methods remain
unsupported until their own complete adapters exist.
