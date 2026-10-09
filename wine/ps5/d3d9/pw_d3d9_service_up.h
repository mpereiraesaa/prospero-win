/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_SERVICE_UP_H
#define PW_D3D9_SERVICE_UP_H
#include <windows.h>
#include "../pw_d3d9_objects.h"
#include "../pw_d3d9_up_wire.h"
void pw_d3d9_service_up_call(struct pw_d3d9_objects *,struct pw_d3d9_object_ref,const struct pw_d3d9_up_request *,struct pw_d3d9_up_reply *);
void pw_d3d9_service_up_retire(struct pw_d3d9_objects *,struct pw_d3d9_object_ref);
void pw_d3d9_service_up_shutdown(struct pw_d3d9_objects *);
#endif
