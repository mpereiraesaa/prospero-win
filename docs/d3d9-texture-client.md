# PE32 pitched texture staging

The client lock helper uses the shared buffer/texture64MiB staging allocator.
After a successful native LockRect it validates the generation, signed pitch,
row count, meaningful row bytes and total span before allocating owned memory
entirely below4GiB. All initial bytes are copied before returning to the game.
For negative pitch it returns the last physical staging row as the first logical
row; the application can step through rows with the original signed pitch.

Unlock copies the full owned span through bounded requests and then returns the
actual backend UnlockRect result. The native adapter skips padding, so partial
edits preserve neighboring pixels outside a subrectangle. READONLY sends no
upload. Allocation/copy errors cancel the native lock and free staging;
malformed replies or unsuccessful cleanup cancel the session. The proxy retains
the object and serializes helper state until cancellation/destruction completes.

The controlled transport fixture runs as actual PE32 code under Wine. It checks
both positive/negative520-byte pitches over more than one ring of data, low-address
spans, partial edits, read-only behavior, read/write failures, malformed layout
cleanup and exhaustion of the shared aggregate budget. Real DXVK row copying is
covered separately by the native texture lab. COM and session integration remain
caller responsibilities.
