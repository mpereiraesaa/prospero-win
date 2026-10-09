/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_SERVICE_TEXTURE_H
#define PW_D3D9_SERVICE_TEXTURE_H
#include <windows.h>
#include "../pw_d3d9_texture_wire.h"
#include "pw_d3d9_kinds.h"
/* Serialized native service ownership. Every texture holds a registry queue
 * reference to its parent device until actual backend texture destruction. */
void pw_d3d9_service_texture_call(struct pw_d3d9_objects *,struct pw_d3d9_object_ref,
 const struct pw_d3d9_texture_request *,struct pw_d3d9_texture_reply *);
HRESULT pw_d3d9_service_texture_destroy(struct pw_d3d9_objects *,uintptr_t);
/* Returns one owned typed COM reference after kind and parent validation. */
HRESULT pw_d3d9_service_texture_acquire(struct pw_d3d9_objects *,struct pw_d3d9_object_ref,
 uint32_t kind,void *native_device,void **);
HRESULT pw_d3d9_service_texture_adopt(struct pw_d3d9_objects *,struct pw_d3d9_object_ref,
 uint32_t kind,void *owned,struct pw_d3d9_object_ref *);
/* All owner operations run in the serialized service, with no concurrent object
 * dispatch. Capture is only immediately after successful Create/Reset; counts
 * come from actual normalized presentation parameters. */
#define PW_D3D9_SURFACE_OWNER_MAX 16u
HRESULT pw_d3d9_service_texture_owners_capture(struct pw_d3d9_objects *,struct pw_d3d9_object_ref,UINT,BOOL);
HRESULT pw_d3d9_service_texture_owners_list(struct pw_d3d9_objects *,struct pw_d3d9_object_ref,struct pw_d3d9_object_ref *,UINT,UINT *);
HRESULT pw_d3d9_service_texture_owners_prepare(struct pw_d3d9_objects *,struct pw_d3d9_object_ref,const struct pw_d3d9_object_ref *,UINT);
HRESULT pw_d3d9_service_texture_owners_begin_reset(struct pw_d3d9_objects *,struct pw_d3d9_object_ref);
/* restore is true only for pinned backend early rejection with TestCoop S_OK.
 * False consumes all parked shells' remote references before destroying entries. */
HRESULT pw_d3d9_service_texture_owners_finish_reset(struct pw_d3d9_objects *,struct pw_d3d9_object_ref,BOOL);
HRESULT pw_d3d9_service_texture_owners_drain(struct pw_d3d9_objects *,struct pw_d3d9_object_ref);
#endif
