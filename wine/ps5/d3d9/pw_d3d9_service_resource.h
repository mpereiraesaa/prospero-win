/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_SERVICE_RESOURCE_H
#define PW_D3D9_SERVICE_RESOURCE_H
#include <windows.h>
#include "../pw_d3d9_resource_wire.h"
#include "pw_d3d9_kinds.h"
/* Serialized native service ownership. Every resource holds a registry queue
 * reference to its parent device until actual backend resource destruction. */
void pw_d3d9_service_resource_call(struct pw_d3d9_objects *,struct pw_d3d9_object_ref,
 const struct pw_d3d9_resource_request *,struct pw_d3d9_resource_reply *);
HRESULT pw_d3d9_service_resource_destroy(struct pw_d3d9_objects *,uintptr_t);
/* Returns one owned typed COM reference after kind and parent validation. */
HRESULT pw_d3d9_service_resource_acquire(struct pw_d3d9_objects *,struct pw_d3d9_object_ref,
 uint32_t kind,void *native_device,void **);
HRESULT pw_d3d9_service_resource_adopt(struct pw_d3d9_objects *,struct pw_d3d9_object_ref,
 uint32_t kind,void *owned,struct pw_d3d9_object_ref *);
#endif
