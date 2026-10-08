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
