# Typed Texture2D and surface COM proxies

The module installs device CreateTexture, CreateOffscreenPlainSurface,
CreateRenderTarget, CreateDepthStencilSurface, UpdateTexture, UpdateSurface,
StretchRect, ColorFill and GetRenderTargetData. It requires the paired texture
protocol v2 helpers, including real DESC level counts and operations 13–17.

Texture/surface wrappers keep canonical local IUnknown/resource identities,
a strong device parent, typed ID/generation references, and a shared private-data
store. Cache keys include parent, kind, ID and generation. Foreign pointer lookup
compares addresses without dereferencing them; copy operations pin both validated
resources while holding the cache lock, then release the lock before RPC.

GetLevelCount comes from actual native metadata. Wrapping an adopted texture with
no level count queries native DESC before publication. GetLevelDesc/GetDesc use
native replies, preserving HRESULTs and output on failure. GetType follows the
actual typed interface kind. GetSurfaceLevel publishes a separately retained
surface through the same canonical cache.

LockRect uses the owned low32 pitched staging client. Only successful locks modify
the caller's locked-rect output. Signed pitch and partial edits are preserved;
READONLY mappings are not uploaded. A busy guard rejects overlapping/reentrant
mapping operations without holding a mutex across RPC. Unlocking the wrong mip
level is rejected without disturbing the active mapping. The current staging
client permits one active mapping per local texture/surface proxy.

Final release removes the cache entry, then retires the native object. Reentrant
retirement queues an intrusive node and retains all staging and parent ownership.
After native retirement/session cancellation owns backend unlocking, local cleanup
zeros the lock generation and frees staging without an extra RPC. Failed enqueue
retains ownership and marks the session failed. No RPC/COM occurs under cache lock.

Shared handles, GetContainer and GetDC are explicitly unsupported. Priority,
PreLoad, LOD and autogen methods are explicit failures until paired native hint
operations are integrated. Private data uses the shared local COM-aware helper.

## Focused proof

`tests/lab/d3d9_texture_proxy.py` builds and runs actual PE32/PE64 controlled-callback
fixtures. The PE32 run exercises positive/negative pitch allocations, partial write
and READONLY copies, wrong-mip rejection, canonical wrapping, private data,
foreign-pointer rejection, all nine device adapters, and deferred final release
while mapped. The PE64 run verifies typed ABI and rejects staging when its allocator
returns an address above the permitted 32-bit range.

Receipt: `/tmp/prospero-d3d9-texture-proxy-r3/receipt.json`.
This proves frontend ABI and ownership with codec round trips. Native helper
execution and production session integration have separate receipts. No console
or gameplay claim is made.
