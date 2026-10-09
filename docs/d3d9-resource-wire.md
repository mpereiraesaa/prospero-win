# Buffer resource payloads

This bounded synchronous payload codec supports vertex/index buffer creation,
description, Lock, copied read/write chunks and Unlock. It is a prerequisite for
the native adapter and PE32 COM proxies, not a complete resource implementation.
The service assigns object IDs in successful creation replies; the outer frame
carries the target device/object/generation. No backend pointer is serialized.

All fields are explicit little-endian integers. Requests start with version,
operation, body length and a zero reserved word; replies start with version,
operation, actual HRESULT and body length. Failed HRESULTs have no output body.
Exact lengths, nonzero transfer generations, chunk limits and offset overflow
are checked before output publication. Unknown operations are unsupported.

Each chunk is at most 4096 bytes, fitting the current 8192-byte session rings.
A successful Lock returns a nonzero 64-bit generation and span capped at 64 MiB.
The native adapter must reject stale generations, bounds violations and reuse,
and keep the real backend lock until Unlock or cancellation cleanup. One active
lock per resource is the initial bounded policy; nested locks are explicitly
unsupported. Zero-size locks expose the remaining safe descriptor range.

The client allocates owned low-address staging, prefills it through READ chunks,
and publishes the complete writable span through ordered WRITE chunks before
Unlock. READONLY locks need no upload. Backend flags pass through unchanged:
DXVK 2.6.2 ignores DISCARD combined with NOOVERWRITE and ignores both for
non-default pools; the codec must not invent errors for these combinations.
The adapter returns the actual backend Lock/Unlock HRESULT. Shared handles are
not represented. Later upload optimizations require separate lifetime/error
proofs.

The focused test round-trips every operation and failure reply, checks exact
bytes and 64-bit generations, rejects every truncated length, oversized chunks,
overflow, zero generations and reserved bits, and verifies failed decoding
leaves output untouched. Actual backend execution is covered by the subsequent
native adapter fixture rather than claimed by these codec checks.
