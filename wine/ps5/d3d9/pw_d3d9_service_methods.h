/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_SERVICE_METHODS_H
#define PW_D3D9_SERVICE_METHODS_H
#include <windows.h>
#include "../pw_d3d9_objects.h"
/* Serialized service call. Zero means malformed typed request/reply. Missing
 * targets are ordinary outer HRESULT failures with zero payload. */
int pw_d3d9_service_methods(struct pw_d3d9_objects *,struct pw_d3d9_object_ref,
 uint32_t,const void *,size_t,void *,size_t,size_t *,HRESULT *);
#ifdef PW_D3D9_ENABLE_BATCH
/* One serialized command-sequence stream per session. A sticky failure forbids
 * further execution, including records after the first failing command. */
struct pw_d3d9_service_batch_state {
    uint64_t next_sequence,failed_sequence;
    uint32_t failed_result,unexpected_result;
    int exhausted;
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
    int bindings;
#endif
};
void pw_d3d9_service_batch_init(struct pw_d3d9_service_batch_state *);
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
/* Set once after HELLO negotiation, before the first batch. Both peers must
 * advertise PW_D3D9_BINDING_FEATURE; compiled support alone is insufficient. */
int pw_d3d9_service_batch_bindings(struct pw_d3d9_service_batch_state *,int);
#endif
int pw_d3d9_service_batch(struct pw_d3d9_service_batch_state *,
 struct pw_d3d9_objects *,struct pw_d3d9_object_ref,
 const void *,size_t,void *,size_t,size_t *,HRESULT *);
#endif
#endif
