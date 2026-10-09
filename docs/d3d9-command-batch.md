# Bounded command batches

This portable codec is a foundation for the approved bridge's queued calls.
It does not enable asynchronous methods, change live transport, reserve an
opcode, or negotiate capabilities. Runtime integration must prove each eligible
method's immediate HRESULT, retain object generations until consumption, order
synchronous boundaries after pending commands, and make unexpected execution
failures sticky. Commands must not be reordered or coalesced: even a render-state
setter can have an ordered resource effect.

A batch applies to the device identified by its future outer transport record.
Its payload is at most8064 bytes, fitting the existing8192-byte ring with framing.
A32-byte little-endian header contains version1, count1..128, first64-bit command
sequence, total payload bytes, and three zero reserved words. Command sequences
are contiguous, nonzero, and cannot wrap. They are distinct from outer reply
tickets. Failed appends neither consume a sequence nor alter the builder.

Each record has a32-bit command length and zero reserved word, followed by the
existing canonical command payload and zero padding to eight bytes. The builder
owns all bytes. The decoder validates the complete batch, including every later
command and padding byte, before publishing output. Execution must never begin
from a partially validated batch. Offset indexes are local metadata and are not
serialized. The accessor accepts only initialized builders or decoded immutable
batches. Encoding buffers must not alias the builder. No guest pointer is sent.

The32-byte acknowledgement contains version, original count and first sequence,
attempted prefix length, first failed index, exact HRESULT and zero reserved
word. Complete success is exactly S_OK, attempted=count, failed_index=UINT32_MAX.
Failure has a failing HRESULT and failed_index=attempted-1; the failing command
was attempted and later commands were not. Unexpected nonzero success results
are unrepresentable and must fail the integration policy, never silently become
S_OK. Receivers reject stale or mismatched ranges and cancel/wake waiters on
unexpected execution failure. Framing does not grant permission to execute a
method; that remains a paired runtime policy. Malformed requests are protocol
failures, not successful empty batches.

The focused test covers owned bytes, scalar/constants batching, every truncation,
malformed later records, padding/reserved fields, count/byte backpressure,
sequence exhaustion, unchanged outputs on errors, and every failure position
for counts1..128. It does not prove queued runtime ordering, method eligibility,
object lifetime, console acceptance or performance.

## Validation-only structural scans

Structural scans use `pw_d3d9_command_validate` to check the same wire rules and
status precedence as the public decoder without constructing a 4,136-byte
command. The shared header parser stores ten 32-bit fields plus a schema pointer
(48 bytes with an eight-byte pointer/alignment). BOOL payloads retain canonical
0/1 validation; all other payload shapes retain their existing opaque-bit rules.
The public decoder still copies payload into its own temporary, validates that
copy, zeroes unused output fields and leaves the complete output untouched on
failure. Overlapping input/output remains supported. Validation alone does not
own caller memory or authorize subsequent execution.

All three batch structural scans remain, including copying the shared input into
owned storage before scanning. Policy, resource pins, declaration preflight,
ordered failure prefixes and replies are unchanged. A successful binding batch
previously materialized each command six times, seven with draw preflight; this
change replaces three of those materializations with validation-only scans.
At source level, each removed pass initialized and copied a 4,136-byte object:
24,816 bytes of logical destination extent per command, or 3,176,448 bytes for
128 commands. These are code-derived extents, not measured memory traffic or CPU
time; compiler optimizations and cache behavior can reduce physical work.

Focused tests compare both new entry points against the frozen previous decoder
for every method, every truncation, byte mutations, maximum vector/BOOL payloads,
status precedence and aliasing. Batch tests retain reserved/padding rejection,
owned-snapshot checks and failure-atomic outputs, and reject noncanonical BOOL
payloads in a suffix before exposing any prefix. No production pair is rebuilt
or deployed by this optimization's host checks.
