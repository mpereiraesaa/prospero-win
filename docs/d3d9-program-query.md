# Native shader and declaration readback protocol

PROGRAM_QUERY_CALL 27 targets an existing typed object (declaration 7, vertex
shader 8, pixel shader 9). SIZE and READ operate on the native immutable object,
including declarations synthesized by SetFVF. They never substitute creation
inputs for backend readback.

Requests contain version, operation, kind, original API capacity, byte offset and
requested byte count. Replies contain version, operation, kind, exact HRESULT,
returned API size, total canonical byte length, offset and copied byte count,
then owned bytes. Capacity/returned size use declaration elements or shader bytes;
offsets/count/total always use canonical bytes. Chunks are at most 4096 bytes,
programs at most 256 KiB, and declarations at most 65 explicit 8-byte elements.

READ preserves the original capacity across chunks and reports the backend size
field. DXVK 2.6.2 GetFunction copies min(capacity, bytecode length), returns success
and leaves capacity unchanged, including zero/partial lengths. GetDeclaration
sets the required count and writes all elements regardless of input count. Guest
adapters must retain these semantics and guard actual guest memory accesses. SIZE
with a NULL data pointer reports the backend required size. Required size pointers
remain required in guest COM adapters. No generic insufficient-buffer HRESULT is
invented by this protocol.

Failed replies contain the backend size field but no data/total/offset. Decoders
validate against the request and leave output untouched on malformed input. The
portable contract covers all kinds, zero/partial/maximum shader reads, declaration
count units, truncation, failed HRESULTs, malformed bounds and atomic outputs.
Native helper, registry pins and PE32 COM integration are separate layers.

Backend semantics reference: [shader](https://github.com/doitsujin/dxvk/blob/v2.6.2/src/d3d9/d3d9_shader.h)
and [declaration](https://github.com/doitsujin/dxvk/blob/v2.6.2/src/d3d9/d3d9_vertex_declaration.cpp).

## Local frontend output publication

Program readback publishes its validated, fully staged byte copy directly to the
caller's local COM output buffer. It does not use WriteProcessMemory: pinned
Wine routes that API through NtWriteVirtualMemory and wineserver's process-memory
writer even for the current process. The PS5 server build has no
`HAVE_PROCESS_VM_WRITEV`; its fallback requires ptrace, which the PS5 compatibility
layer rejects with EPERM. That path can reject a valid local stack output buffer.

The declaration input count remains ignored, matching the pinned backend. Shader
readback retains its byte-capacity semantics. Nothing is copied to the caller
until every requested chunk has succeeded and matched the immutable total.

`tests/lab/d3d9_program_copy.py` runs the actual PE32 and PE64 program COM proxy,
device frontend, and wire codecs with controlled successful backend replies and
a denied WriteProcessMemory hook. The unfixed control reproduces INVALIDCALL with
untouched output; the corrected path copies successfully without calling the
hook. Both cover poisoned declaration counts, shader zero/partial/multiple-chunk
reads, and a second-chunk error leaving all caller bytes unchanged. These are
controlled frontend proofs, not console or native-backend acceptance.
