# Generated Vulkan input snapshots

The codec generator uses the same pinned XML and Wine type model as Wine's
Vulkan thunk generator. It emits a fieldwise encoder/decoder and one roundtrip
fixture per void input thunk, plus a coverage manifest. This dependency does
not enable new runtime batching by itself.

Pointers are represented by presence tags and owned contents. Dispatchable
handles and `size_t` are widened explicitly; descriptor image, buffer and texel
members are selected by descriptor type. Inactive descriptor members are never
read. Registered input extension chains, selector unions, fixed/dynamic arrays,
strings, nested pointer arrays and video bitfields are normalized. The opaque
checkpoint marker is an identity token that Vulkan never dereferences.

Scalar/count/selector fields precede pointer fields in the packet. This also
handles structs whose XML lists an array before its count. Recursion, input
extent, integer overflow, alignment and caller/wire aliasing are bounded.
Unknown chain types, callback structures and input blobs without a documented
extent fail encoding before enqueue. `VkCuLaunchInfoNVX.pParams/pExtras` cannot
be deep-copied because the API does not provide individual blob sizes.

Immediate-output void getters require synchronous handling. Descriptor-template
blob codecs need the existing template metadata cache. Manual PE destruction
needs deferred client-allocation retirement before it can become asynchronous.
These categories and the exact unresolved structures appear in the manifest;
a generated function is not a claim that every extension shape is supported.

`python3 tests/test_vk_codecs.py` runs portable bounds/ownership tests, including
ASan/UBSan. `tests/lab/vk_codecs_runtime.py` accepts Wine source/build and XML
paths and compiles real PE32 and Unix64 layouts under an 8 GiB memory cap. Its
fixtures overwrite source storage after encoding, decode/re-encode every viable
opcode, reject truncated/trailing packets, and compare PE32 bytes against the
native parameter fixtures. No driver or console is involved.

Client-object destruction requires a separate retirement step. The staged manual
instance, device, pool and command-buffer wrappers retain their CRT allocations
until global replay completes; PE list unlinking stays immediate. Retirement
node allocation failure drains first, then releases the client object. Other
loader frees, including failed creation cleanup, remain immediate. This hook is
a dependency for generated destruction codecs; it does not itself enable them.

The v2 opt-in runtime uses 256 KiB producer arenas. A generated record carries
the original Unix function index followed by owned fieldwise input bytes; replay
decodes into aligned native parameter storage and calls the native thunk, never
the guest-pointer thunk. Entire batches pass semantic preflight before effects.
The Unix entry accepts legacy v1 batches, while a new PE requires the exact v2
capability; mismatched old Unix libraries disable batching safely. Existing
seven fast codecs stay first. Additional generated operations are enabled only
with the default all-category mask (127); narrower diagnostic masks retain their
previous scope. Unknown shapes drain and dispatch synchronously. All six descriptor-template forms use device-scoped registered metadata, with
command-buffer client tails retaining the originating device association.

The process scratch limit is 16 MiB to preserve the existing 63-producer bound
with larger arenas. Unix decoding uses a bounded 4 MiB allocation per entry.
These allocation and memory costs require measurement before default enablement.

The codec exposes an injected descriptor-template snapshot callback. Encoding
uses registered entry extents; decoding checks the normalized header, template
identity, byte extent and reserved fields before allocating aligned owned data.
The callback is producer state and is never serialized. Generated fixtures cover
all six core/KHR template forms with explicit uniform-buffer metadata. The
actual Unix-entry fixture uses controlled native thunks to verify owned nested
barriers, destruction ordering, piggyback status propagation and full-batch
semantic rejection before effects; it does not exercise a GPU driver.

Template metadata supports both descriptor-set and push-descriptor template
kinds, including empty templates. Sparse image, buffer, texel-buffer and
acceleration-structure spans are copied; inline uniform bytes use contiguous
extents and ignore the API stride. Mutable descriptor types, unknown metadata,
creation extension chains and allocator callbacks take the synchronous fallback
path. Command-buffer allocation writes only a PE tail association; the shared
client-object prefix used by Unix unwrapping stays unchanged.

The public Wine build fingerprint includes the codec generator as well as all
adapter sources, so a generator change reconfigures the staged module instead
of silently retaining an older generated Makefile or codec. XML inputs use the
exact registry revision declared by the pinned Wine generator; explicit XML
paths remain available for offline fixture reproduction.

The PE runtime lab fixture now exercises both core and KHR template creation
through the real producer metadata hook, then queues all six update/push forms.
It overwrites descriptor and nested barrier inputs, retires template metadata
before replay, and checks that a deferred device client prefix remains live
until replay completes. The Unix boundary is controlled; these checks establish
producer ownership and lifetime without claiming graphics-driver execution.

Replay now starts with a private aligned 4 KiB decode arena per Unix entry.
Legacy-only and ordinary small generated batches allocate no decode heap.
A decode that cannot fit retries once at the existing 4 MiB bound, then reuses
that allocation for the remainder of semantic preflight and replay. Each entry
owns its context, including recursive and concurrent entries. This removes
unconditional 4 MiB allocation/free per batch without weakening full-batch
validation or reducing supported input sizes. The Unix-entry fixture counts
allocations, tests exact bounds, growth failure, recursion and parallel calls,
and offers a host microbenchmark of nested barriers, viewports and descriptor
updates. Host timings do not establish a console frame-rate improvement.

Same-width primitive arrays now copy their contiguous scalar representation in
one bounded codec operation. Records, callbacks and pointer-size values retain
fieldwise conversion, including PE32-to-Unix64 widening. The shared helper checks
count multiplication, source aliasing and remaining wire capacity before copying.
The cross-layout fixture continues to exercise every supported generated schema;
host throughput measurements carry no native graphics performance claim.
The optional `blob-benchmark` mode runs a 64 KiB buffer update through encoding,
stream collection, both decode passes and controlled native dispatch, counting
generated scalar codec calls without asserting elapsed time.

## Multidraw byte strides

`vkCmdDrawMultiEXT` and `vkCmdDrawMultiIndexedEXT` accept a byte stride between
input records. Encoding walks that stride, owns each record, and serializes a
packed stride. Caller arrays and parameters stay unchanged. This uses the existing
wire fields and works with the existing Unix decoder; no Unix library replacement
is needed. Multiple records require an aligned stride at least the record size;
a single record accepts zero stride. Pointer overflow and wire aliasing fail before
source reads. Indexed draws retain the optional shared vertex offset and skip the ignored
per-record vertex offset when that shared value is present.

The PE32-to-Unix64 lab fixture checks the values at the decoded driver stride for
empty, single, tightly packed and padded arrays, both indexed offset modes, source
overwrite, malformed strides, pointer overflow and truncated packets. No GPU is
used. Contracts: [multidraw](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDrawMultiEXT.html)
and [indexed multidraw](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDrawMultiIndexedEXT.html).
