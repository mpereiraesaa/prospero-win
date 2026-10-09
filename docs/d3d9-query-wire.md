# Query payloads

Opcode29 uses service query kind11. The fixed little-endian32-byte header carries
no pointers. Requests distinguish NULL output from nonnull zero-size output and
preserve native DWORD flags. Data requests copy up to16 caller bytes; replies
return exactly that span even for S_FALSE or failed backend HRESULTs. This preserves
partial backend writes and untouched bytes without exposing native stack storage.

The initial POD family is VCACHE, EVENT, OCCLUSION, TIMESTAMP, TIMESTAMPDISJOINT and
TIMESTAMPFREQ. Native support probes still call CreateQuery with a NULL output;
actual object type and data size are checked when creating an object. Requests
larger than the actual data size are rejected to prevent backend cache overreads.
Other query families are explicitly unavailable in this adapter.

Portable coverage checks exact framing, truncation, malformed sizes and pending/
failed output payloads. Native backend and COM frontend proofs are separate.
