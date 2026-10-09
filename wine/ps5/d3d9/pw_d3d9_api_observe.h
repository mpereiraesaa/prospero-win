/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_API_OBSERVE_H
#define PW_D3D9_API_OBSERVE_H
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
/* Opt-in once per process. Disabled publication returns the original table.
 * An enabled interface accepts exactly one immutable source table; a different
 * table returns NULL. Install after all typed slots have been populated. */
int pw_d3d9_api_observe_enabled(void);
int pw_d3d9_api_diagnostics_enabled(void);
/* Entry origin, not nesting depth, separates guest calls from bridge pins.
 * Every method count is atomic; a snapshot is not a global stop-the-world cut.
 * Entries saturate at LLONG_MAX and aggregates at UINT64_MAX; saturated=1
 * marks lost numeric precision without wrapping or negative histogram deltas. */
#define PW_D3D9_API_INTERFACES 17
#define PW_D3D9_API_SLOTS 119
struct pw_d3d9_api_profile_snapshot {
 uint64_t entries[PW_D3D9_API_INTERFACES][PW_D3D9_API_SLOTS];
 int enabled,classification_valid,saturated;
};
void pw_d3d9_api_profile_enter(unsigned,unsigned,const void *);
void pw_d3d9_api_profile_snapshot(struct pw_d3d9_api_profile_snapshot *);
/* Optional transport hook AFTER unlocking: epoch, original request sequence,
 * object, generation, final HRESULT. Process-wide mixed-device counts; first
 * boundary includes startup and failed Present attempts also delimit intervals.
 * Local/reentrant rejections without a completed unlocked transport boundary
 * count as API entries but do not invoke this emitter.
 * Histograms emit at boundary 1 and every 120, with their own interval length.
 * Only externally originated vtable calls into this DLL count. Factory exports
 * and DLL-local helper/pin calls are excluded; guest callback reentry counts.
 * PW_D3D9_PROFILE=1 requires the observer to be compiled/published. Diagnostics
 * remains independently controlled by PW_D3D9_DIAGNOSTICS. Neither enabled means
 * original vtables, zero entry hooks, and zero profiling clock reads. */
void pw_d3d9_api_profile_present(uint32_t,uint64_t,uint32_t,uint32_t,HRESULT);
/* Unlocked session teardown boundary, not a process-final or Present claim. */
void pw_d3d9_api_profile_flush(uint32_t epoch);
/* Identity-safe lookup; never calls COM or dereferences the supplied table. */
const void *pw_d3d9_api_original_vtable(const void *);
void pw_d3d9_api_failure(const char *,unsigned,const char *,HRESULT,void *,REFIID);
enum pw_d3d9_api_field_kind { PW_D3D9_API_WORDS, PW_D3D9_API_POINTER, PW_D3D9_API_LOCKED_RECT };
struct pw_d3d9_api_field {
 const char *name;const void *address;SIZE_T bytes;enum pw_d3d9_api_field_kind kind;
};
int pw_d3d9_api_sample(LONG *);
int pw_d3d9_api_read(void *,const void *,SIZE_T);
void pw_d3d9_api_output(const char *,unsigned,const char *,HRESULT,void *,
 const struct pw_d3d9_api_field *,unsigned);
#include "pw_d3d9_api_observe_generated.h"
#ifdef PW_D3D9_ENABLE_API_OBSERVE
#define PW_D3D9_API_OBSERVE(type,table) ((type##Vtbl *)pw_d3d9_api_observe_##type(table))
#else
#define PW_D3D9_API_OBSERVE(type,table) (table)
#endif
#endif
