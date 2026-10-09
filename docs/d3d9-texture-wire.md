# Texture and surface upload payloads

The synchronous texture payload family carries CreateTexture2D,
CreateOffscreenPlainSurface, descriptions, GetSurfaceLevel, LockRect, copied
read/write chunks, UnlockRect, cancellation, AddDirtyRect, UpdateTexture and
UpdateSurface. The outer session frame selects the target object or device.
Copy requests use typed object ID/generation pairs; creation replies carry
service-assigned identities. No HWND, COM or mapped-memory pointer is serialized.

Fields are explicit little-endian scalars. Optional rectangles/points use a
Boolean presence field and canonical zero coordinates when absent. Layouts
include signed backend pitch, number of pixel/block rows, meaningful row bytes
and checked storage length `abs(pitch)*(rows-1)+row_bytes`, bounded at64MiB.
Chunk limits remain4096 bytes. Padding is zero in staging and must not be read
from or written into native row padding. Negative pitch requires the client to
return its last staging row as the logical first row. Compressed formats count
block rows and block bytes, not pixels.

The native adapter remains responsible for format/block alignment, descriptor
bounds, exact backend HRESULTs, stable subresource COM identity and active lock
alias handling. This codec does not claim backend support for every format.
Failed replies carry no outputs; malformed lengths/generations/layouts and
unknown methods are rejected before publishing decoded output.

Focused checks round-trip all12 operations and failure replies, every truncated
length, signed pitches, sparse optional fields, generation and arithmetic
boundaries. Actual backend and PE32 staging proofs belong to adapter changes.
