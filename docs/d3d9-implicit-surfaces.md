# Implicit default-surface ownership

The texture helpers provide an explicit device-owner lifetime for actual default
backbuffers and the initial auto-depth surface. The adapter captures their IDs
immediately after successful CreateDevice or Reset, using the backend's normalized
presentation parameters. Later GetRenderTarget or GetDepthStencilSurface results
are not automatically classified as defaults: they may be application resources.

A local surface shell with a default owner survives public Release returning zero.
It retains its remote shell reference and canonical cache entry, but drops its
strong local device reference. Public reacquisition restores that device reference
under the same cache lock used for final device retirement. This prevents both a
device/child reference cycle and resurrection after final device cleanup begins.
Final device decrement closes admission and leaves a private teardown sentinel;
the device adapter drains children and frees the device directly afterward.

## Reset transaction

The frontend snapshots public-zero default shells and marks them frozen under its
cache lock, then releases the lock before transport or native work. Frozen shells
reject AddRef, QueryInterface, and pointer resolution without blocking. Positive
public references remain usable and retain their native storage reference. A
positive snapshot that reaches zero during Reset keeps that storage until the
transaction completes. No cache lock is held across a transport call.

The service validates the snapshot, then parks only public-zero native surface
storage references immediately before backend Reset. Registry identities and
local shell allocations remain reserved. An early rejected Reset that preserved
the original native owners restores storage and thaws the same generations.
Successful Reset, or a later failure after default-owner destruction, abandons
parked weak addresses without dereferencing them, consumes their remote shell
references, and destroys those registry entries before capturing new defaults.
Positive-public old surfaces lose the device-owner lease but retain their normal
public ownership. The backend Reset outcome must be captured before subsequent
window mirroring, whose separate failure cannot undo successful native Reset.

This preservation rule is based on the pinned DXVK implementation: early
presentation-parameter validation returns with TestCooperativeLevel S_OK; later
failure after ResetState/default destruction leaves NotReset. Service orchestration
must use the actual backend outcome, not infer preservation from the final
adapter HRESULT. Cancellation must never restore a weak pointer of uncertain
validity. Ordinary registry cancellation can destroy parked contexts without
releasing their weak addresses.

The helper additions are dormant until the device/session adapter installs owner
lists and orchestrates these boundaries. They do not implement private references
for arbitrary bound resources or texture-container subresources. Those ownership
families require their own proven owner boundaries. Compile checks alone do not
establish runtime or console acceptance.
