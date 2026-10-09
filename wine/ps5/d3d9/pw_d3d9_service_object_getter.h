/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_SERVICE_OBJECT_GETTER_H
#define PW_D3D9_SERVICE_OBJECT_GETTER_H
#include <windows.h>
#include "../pw_d3d9_objects.h"
#include "../pw_d3d9_object_getter.h"
void pw_d3d9_service_object_getter(struct pw_d3d9_objects *,struct pw_d3d9_object_ref,
 const struct pw_d3d9_object_getter_request *,struct pw_d3d9_object_getter_reply *);
#endif
