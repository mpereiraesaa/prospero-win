/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_WINDOW_H
#define PW_D3D9_WINDOW_H
#include <stdint.h>
#define PW_D3D9_WINDOWS 64u
#define PW_D3D9_WINDOW_LEASES 64u
#define PW_D3D9_WINDOW_VISIBLE 1u
#define PW_D3D9_WINDOW_FOCUSED 2u
#define PW_D3D9_WINDOW_FULLSCREEN 4u
struct pw_d3d9_window_id { uint32_t epoch,id,generation; };
struct pw_d3d9_window_state { int32_t x,y;uint32_t width,height,flags; };
struct pw_d3d9_window_lease { uint32_t epoch,id,generation; };
struct pw_d3d9_window_entry {
 uint64_t guest,service,sequence,pending_sequence;
 uint32_t generation,live,closing,failed,backend_result,leases;
 struct pw_d3d9_window_state applied,pending;
};
struct pw_d3d9_window_lease_entry { uint32_t generation,live;struct pw_d3d9_window_id owner; };
struct pw_d3d9_windows {
 uint32_t epoch;
 struct pw_d3d9_window_id plane_owner;
 struct pw_d3d9_window_entry windows[PW_D3D9_WINDOWS];
 struct pw_d3d9_window_lease_entry leases[PW_D3D9_WINDOW_LEASES];
};
enum pw_d3d9_window_result { PW_D3D9_WINDOW_OK, PW_D3D9_WINDOW_INVALID,
 PW_D3D9_WINDOW_STALE, PW_D3D9_WINDOW_BUSY, PW_D3D9_WINDOW_EXHAUSTED,
 PW_D3D9_WINDOW_CLOSED, PW_D3D9_WINDOW_BACKEND, PW_D3D9_WINDOW_HIDDEN };
/* Caller serializes every operation. No function allocates, waits, calls Wine,
 * invokes callbacks, or takes a lock. Invoke UI messages outside caller locks.
 * Only id/epoch/generation and explicit state scalars belong on the wire.
 * guest/service are local opaque handle identities, NEVER serialized pointers.
 * The native adapter must validate process/thread/domain ownership first. */
int pw_d3d9_windows_init(struct pw_d3d9_windows *,uint32_t epoch);
int pw_d3d9_window_attach(struct pw_d3d9_windows *,uint64_t guest,uint64_t service,struct pw_d3d9_window_id *);
/* Distinguishes an unassociated ordinary HWND from a suppressed paired HWND. */
int pw_d3d9_window_find(const struct pw_d3d9_windows *,uint64_t local_handle,struct pw_d3d9_window_id *);
int pw_d3d9_window_get(const struct pw_d3d9_windows *,struct pw_d3d9_window_id,struct pw_d3d9_window_entry *);
/* Ordered two-phase mirror. No presentation until first successful ack. */
int pw_d3d9_window_begin(struct pw_d3d9_windows *,struct pw_d3d9_window_id,uint64_t sequence,const struct pw_d3d9_window_state *);
int pw_d3d9_window_ack(struct pw_d3d9_windows *,struct pw_d3d9_window_id,uint64_t sequence,uint32_t hresult);
/* Reserve before display enumeration/release; cancel on failed surface create.
 * Multiple leases from the SAME pair allow create-new-before-destroy-old reset.
 * Unknown probes and guest-side surfaces can never acquire a bridge lease. */
int pw_d3d9_window_acquire(struct pw_d3d9_windows *,struct pw_d3d9_window_id,uint64_t service,struct pw_d3d9_window_lease *);
int pw_d3d9_window_release(struct pw_d3d9_windows *,struct pw_d3d9_window_lease);
/* Close blocks admission; pending mirror must ack and leases must be released
 * before detach. A stale release cannot affect a later association. */
int pw_d3d9_window_close(struct pw_d3d9_windows *,struct pw_d3d9_window_id);
int pw_d3d9_window_detach(struct pw_d3d9_windows *,struct pw_d3d9_window_id);
/* Local driver input route: paired service always maps to the guest owner.
 * A closing/failed/hidden association returns zero (do not route input). */
uint64_t pw_d3d9_window_input(const struct pw_d3d9_windows *,uint64_t local_handle);
#endif
