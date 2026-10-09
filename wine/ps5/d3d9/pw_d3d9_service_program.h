/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_SERVICE_PROGRAM_H
#define PW_D3D9_SERVICE_PROGRAM_H
#include <windows.h>
#include "../pw_d3d9_objects.h"
#include "../pw_d3d9_program_wire.h"
#include "../pw_d3d9_program_query.h"
void pw_d3d9_service_program_call(struct pw_d3d9_objects *,struct pw_d3d9_object_ref,const struct pw_d3d9_program_request *,struct pw_d3d9_program_reply *);
void pw_d3d9_service_program_query(struct pw_d3d9_objects *,struct pw_d3d9_object_ref,const struct pw_d3d9_program_query_request *,struct pw_d3d9_program_query_reply *);
void pw_d3d9_service_program_shutdown(struct pw_d3d9_objects *);
void pw_d3d9_service_program_retire(struct pw_d3d9_objects *,struct pw_d3d9_object_ref);
HRESULT pw_d3d9_service_program_destroy(struct pw_d3d9_objects *,uintptr_t);
HRESULT pw_d3d9_service_program_acquire(struct pw_d3d9_objects *,struct pw_d3d9_object_ref,uint32_t,void *,void **);
HRESULT pw_d3d9_service_program_adopt(struct pw_d3d9_objects *,struct pw_d3d9_object_ref,
 uint32_t kind,void *owned,struct pw_d3d9_object_ref *);
#endif
