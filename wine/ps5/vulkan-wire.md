# Normalized Vulkan payloads and descriptor-template metadata

These codecs are the second dependency of the 32-bit Vulkan command stream.
They are not yet installed into Wine's dispatch path. The typed Wine adapters
and global flush/runtime lifecycle remain separate work.

Stable wire opcodes cover indexed draw, pipeline bind, index-buffer bind,
descriptor-set bind, vertex-buffer bind with optional sizes/strides, and descriptor
template updates, plus copied push-constant values. These cover the seven hot
categories identified in the retained release thunk table. Fields use explicit little-endian byte offsets; dispatchable
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

## Runtime integration

`tools/stage_vk_batch.py` stages the portable dependencies and 32-bit PE /
Unix runtime into the patched Wine source before configure. Generated Unix
tables append one private batch entry without changing existing indices.
Capability is negotiated through an existing function-availability entry
with a live instance; older Unix modules return zero for the private name,
so a new PE module never indexes their missing batch entry.

`PW_VK_BATCH=1` enables the seven implemented command categories. Every
unbatched operation drains all producers, then executes the ordinary call
in the same Unix crossing. The process ordering gate spans recording and
replay, while input payloads and template metadata are deep-owned.
Debug callbacks, custom allocators, configured layers, and opaque instance
creation chains disable batching permanently after draining pending work;
subsequent calls bypass the gate so ordinary callback reentry remains valid.

`PW_VK_BATCH_STATS=1` independently enables process-scoped Wine crossing
and replay counters at present boundaries. Keep this unset for an
uninstrumented FPS run. It does not count every native WoW64 transition.

The 32-bit DLL's thread-detach hook only marks its producer retired; a
subsequent global drain replays and frees its owned arena without taking
a mutex or calling the driver under the loader lock. The 64-bit DLL retains
its original DisableThreadLibraryCalls behavior and direct Unix dispatch.
Host PE controls exercise the actual retirement helper, with automatic DLL
notification wiring checked separately in the staged loader source.
