# Native texture and surface adapter

The native service owns real Texture2D and Surface9 interfaces. It creates
textures/offscreen surfaces, returns level descriptions and canonical surface
identities, locks and copies rows, marks dirty rectangles and forwards
UpdateTexture/UpdateSurface with actual HRESULTs. Registry integration must
retain the parent device/window and deduplicate returned surface identities.

The initial lock format set includes common8/16/24/32-bit color/luminance formats
and DXT1–5. Unknown formats and unsupported block-alignment cases return an
explicit unsupported result. Descriptor bounds are checked before LockRect;
backend flags are passed unchanged. The real signed pitch is validated against
meaningful row bytes and block-row count. Storage is bounded at64MiB.

Transfers copy only meaningful row bytes. Reads return zero padding and writes
skip native padding, preserving pixels outside a subrectangle. Negative pitch
maps staging rows in physical address order; the client returns the last staging
row as its first logical row. Full writable spans must arrive contiguously before
UnlockRect. READONLY prohibits writes; cancellation releases incomplete locks.

A bounded128-entry table compares canonical surface identities so locking the
same underlying image through both a texture and its Surface9 alias cannot
create independently writable mappings. Table access holds a short SRW lock;
backend COM calls execute outside it. Cleanup always removes the reservation and
releases the retained surface. The caller serializes each context. Unexpected
UnlockRect failure is returned while owned resources are still released.

The actual host lab covers ARGB8 subrectangle preservation, DXT1/3/5 uploads and
readback, full lock-table exhaustion and cleanup, stale-generation/range
rejection, surface identity and active alias rejection, dirty rectangles,
cancellation, active-lock destruction, UpdateTexture and UpdateSurface.
A separate controlled storage case exercises the real negative-pitch copier and
padding preservation. Three processes must finish cleanly. This is native adapter
coverage, not a PE32 texture COM proxy or console game acceptance claim.

The adoption entry consumes an already-owned backend texture/surface reference
on every path, validates the requested interface and GetDevice ownership, and
retains the actual backend device. This handles implicit backbuffers/depth
surfaces and resources recovered from backend bindings after the original guest
proxy is released. The fixture proves backbuffer/render-target identity reuse,
depth adoption, wrong-interface rejection and retained texture binding adoption.


## Render targets, depth surfaces and StretchRect

Texture protocol version 2 adds operations 13 CreateRenderTarget, 14
CreateDepthStencilSurface and 15 StretchRect. Multisample type/quality and native
BOOL lockable/discard bits pass to the actual backend. StretchRect resolves two
same-device surface contexts and reconstructs independent optional source and
destination rectangles plus the native filter enum. No shared handles are sent.
All existing operation numbers remain stable, but both peers must deploy v2.

DESC now includes actual GetLevelCount for textures and 1 for surfaces, enabling
correct adopted texture proxies. Wire encoders/decoders reject the old layout.
The native fixture creates render/depth surfaces, verifies a scaled subrectangle
copy through changed pixels and untouched outside pixels, compares a full-surface
copy's HRESULT directly, and queries a genuine six-level texture chain.


ColorFill (operation 16) targets a typed surface context with optional rectangle
and exact native D3DCOLOR bits. GetRenderTargetData (17) resolves two same-device
surface contexts and invokes the real backend. Neither operation uploads guest
addresses. The fixture fills a render target and a subrectangle, reads it back to
a system-memory surface, then verifies both colors through the copied lock path.
An invalid ColorFill rectangle also compares its HRESULT with a direct call.
