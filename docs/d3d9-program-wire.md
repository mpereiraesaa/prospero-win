# D3D9 shader and declaration uploads

The portable program codec transfers owned bytes for vertex declarations (kind
7), vertex shaders (8) and pixel shaders (9). The enclosing frame authenticates
the session epoch and target device. It never contains guest pointers. This is a
codec and per-device assembler; production proxy and native backend integration
must call them explicitly.

Requests use eight little-endian DWORDs: version 1, operation, kind, total bytes,
offset, chunk count, transfer generation low/high. WRITE appends exactly count
bytes, up to 4096. BEGIN declares a total of at most 256 KiB, with no transfer ID.
The service assigns a monotonically increasing 64-bit transfer generation.
Only one upload is active per device. WRITE must start at the exact next offset;
missing, overlapping, reordered and stale chunks fail. COMMIT requires all bytes
and validates the complete program. Duplicate COMMIT cannot create twice. The
caller retains the buffer through backend creation and then calls finish on
success or failure. ABORT retires an active transfer. IDs never wrap. Device
teardown discards its upload, and a new device has a different outer object ID.

Replies are eight DWORDs: version, operation, exact HRESULT, object ID,
object generation, reserved zero, transfer generation low/high. Successful
COMMIT returns a nonzero service object ID/generation; other successes return
only the transfer ID. Failed HRESULT replies have no output fields. Registry
publication must succeed before returning a successful creation reply. The
native caller must release a newly created object if publication fails.

Shader tokens are explicit little-endian DWORDs. Measurement reads a version,
then walks instruction boundaries until the exact END token. SM2/3 instruction
lengths and comment lengths skip payload tokens; END-shaped float constants or
comment words cannot truncate the program. SM1 uses fixed opcode parameter
counts including the PS1.4 texture operand differences. Unknown SM1 instructions
fail closed. This is structural measurement, not semantic shader validation:
the real D3D9 backend owns shader legality and its HRESULT. The token reader
must check the caller's accessible span or catch read faults. Supplying a large
maximum does not establish that a guest pointer is readable. Measurement does
not publish a length on failure. Wire validation requires END to be the final
word, rejecting trailing data and incomplete instructions.

Declaration bytes encode stream and offset as little-endian WORDs followed by
type, method, usage and usage-index bytes. At most 64 elements plus the canonical
D3DDECL_END sentinel are accepted. The sentinel must be last. Backend validation
still determines semantic combinations, stream alignment and device support.

Assembler access requires external serialization. Its caller-owned storage must
outlive backend creation. Inputs and outputs must not overlap. Invalid requests
leave upload state unchanged. Invalid/truncated codec input leaves output DTOs
unchanged. Payload sizes are checked before copying; oversize programs fail
explicitly rather than truncating. Backend creation and object publication are
not performed by the portable assembler.

`test_d3d9_program_wire` covers SM1/SM3 boundaries, embedded END words, malformed
and truncated payloads, canonical declarations, exact failed HRESULTs, maximum
size chunk assembly, wrong offsets, stale generations, abort, generation
exhaustion and duplicate commit. Console game compatibility remains a separate
gate from these host contracts.
