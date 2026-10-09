/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_SERVICE_QUERY_H
#define PW_D3D9_SERVICE_QUERY_H
#include <windows.h>
#include "../pw_d3d9_query_wire.h"
/* Serialized service dispatch. CREATE targets device; Issue/GetData target kind11.
 * Returned native backend errors still carry seeded/partially written data bytes. */
void pw_d3d9_service_query_call(struct pw_d3d9_objects *,struct pw_d3d9_object_ref,const struct pw_d3d9_query_request *,struct pw_d3d9_query_reply *);
HRESULT pw_d3d9_service_query_destroy(struct pw_d3d9_objects *,uintptr_t);
#endif
