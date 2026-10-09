/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_SERVICE_STATEBLOCK_H
#define PW_D3D9_SERVICE_STATEBLOCK_H
#include <windows.h>
#include "../pw_d3d9_stateblock_wire.h"
void pw_d3d9_service_stateblock_call(struct pw_d3d9_objects *,struct pw_d3d9_object_ref,const struct pw_d3d9_stateblock_request *,struct pw_d3d9_stateblock_reply *);
HRESULT pw_d3d9_service_stateblock_destroy(struct pw_d3d9_objects *,uintptr_t);
/* Accept a scratch capacity while the fixed wire encoder requires exactly16.
 * Output byte count is published only after successful encoding. */
int pw_d3d9_service_stateblock_reply(void *,size_t,size_t *,const struct pw_d3d9_stateblock_request *,const struct pw_d3d9_stateblock_reply *);
#endif
