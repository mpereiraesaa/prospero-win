# Native gamma forwarding

The service pins the real device while this helper forwards SetGammaRamp or
GetGammaRamp. Native NULL pointers, full swapchain indices and Set flags are
preserved. Owned local D3DGAMMARAMP storage is seeded from copied caller data, so
backend no-op Get calls leave output unchanged. A successful status only means the
void native method was called; no native HRESULT or cached gamma is invented.

The actual DXVK PE64 fixture passes in three processes. It compares real Get state,
sets channel-distinct ramps, verifies real readback, checks invalid-index no-ops
and NULL calls, and restores the original gamma before device teardown. This is
host backend evidence, not console or session integration acceptance.
