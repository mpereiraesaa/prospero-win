# Normalized Vulkan payloads and descriptor-template metadata

These codecs are the second dependency of the 32-bit Vulkan command stream.
They are not yet installed into Wine's dispatch path. The typed Wine adapters
and global flush/runtime lifecycle remain separate work.

Stable wire opcodes cover indexed draw, pipeline bind, index-buffer bind,
descriptor-set bind, vertex-buffer bind with optional sizes/strides, and descriptor
template updates. Fields use explicit little-endian byte offsets; dispatchable
client handles use zero-extended 32-bit values, and non-dispatchable handles keep
all 64 bits. The surrounding command stream provides version/sequence framing.

Binding arrays are copied during encoding. Payloads retain no array pointers.
Output and input ranges must not overlap; encoders reject aliasing. All encoded
payloads have a one MiB upper bound and reject count/extent arithmetic overflow.
The initial Wine adapter may choose a smaller record limit and flush/fall back
for records exceeding it. Host decoding needs a complete payload validation pass
before executing any command, in addition to transport framing preflight.

Descriptor templates require metadata from successful creation. The cache owns
entry copies keyed by client device plus the full template handle. It initially
supports descriptor-set templates, types 0 through 10, no pNext and zero flags.
Unknown metadata, unsupported descriptor types or allocation failure makes that
template unbatchable. Unsupported successful handle reuse removes stale metadata;
failed creation does not alter a previously known object.

At enqueue, snapshot computes a bounded extent from offset/count/stride and copies
only referenced descriptor spans. Gaps and image-info padding are zeroed. Caller
storage can be overwritten immediately afterward. The existing Wine thunk also
forwards template data without structural conversion; these supported value-only
layouts keep that contract while making the bytes owned. Acceleration structures,
inline uniform blocks, mutable descriptors and unknown extensions use fallback.

The caller supplies allocation and holds the process synchronization gate from
metadata lookup through encoding and ordered append. Lookup returns a borrowed
entry view valid only inside that protected operation. A global drain precedes
metadata retirement and the original template/device destruction call, with the
replay-order gate held until that immediate operation completes. Core/KHR creation
and destruction interception is not implemented by these portable helpers.

Run `python3 tests/test_vk_wire.py --output /tmp/pw-vk-wire-tests` for host and
ASan/UBSan tests plus i686 Windows compilation. Checks cover sparse data copying,
caller storage reuse, 64-bit handles, signed vertex offsets, optional arrays,
aliasing, capacity/address bounds, unsupported types and template lifecycle.
This verifies local codecs, not driver semantics, Wine integration or game FPS.
