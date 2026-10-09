# Native shader and declaration creation

The native helper consumes the complete canonical bytes produced by the program
upload assembler. It validates the payload again and reconstructs native DWORD
tokens or native D3DVERTEXELEMENT9 fields before invoking the actual device's
CreateVertexShader, CreatePixelShader or CreateVertexDeclaration method.

It returns the backend HRESULT and a local context only on successful creation.
The context owns the created COM object and an explicit parent device reference.
Destroy releases both. Identity queries use canonical IUnknown; backend pointers
are local service-only values. The service dispatcher must publish an opaque
registry ID/generation, retain the parent device/window context, and release this
context if publication fails. No raw COM pointer may be placed in a wire reply.

The per-device upload is finished after the creation attempt regardless of its
HRESULT, so duplicate commits cannot create duplicate objects. The helper does
not mutate upload state or perform registry publication itself. Calls require
service-thread serialization and a live typed device supplied by the registry.

`tests/lab/d3d9_native_program.py` compiles a real PE64 fixture and executes three
processes using the supplied host Wine and native D3D9 backend. Each process
creates a real device, uploads a declaration, a VS1.1 shader and a PS2.0 shader
with a multi-chunk comment, binds/unbinds the resulting objects and checks
GetDeclaration/GetFunction readback. Caller staging is overwritten before
readback. The shader readback queries its required byte count with null data
before providing storage, respecting the backend's GetFunction contract.

The fixture also rejects a truncated program before backend creation. Receipts
record source, executable and backend hashes. This proves the native helper and
copied payload path; production PE32 COM proxies and console game compatibility
are separate integration gates.

Object-returning getters may expose implicit FVF declarations or objects kept
alive by native bindings after the last guest reference. The adopt entry point
consumes one owned backend reference on every path, queries the exact interface,
and verifies the actual GetDevice result before publishing a local context.
It retains the GetDevice reference as parent ownership. The registry must still
deduplicate canonical identity. The real fixture checks adoption, canonical
identity equality and rejection of an unsupported kind for each object type.
