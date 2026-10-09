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
