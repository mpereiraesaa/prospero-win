/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_NATIVE_RESOURCE_H
#define PW_D3D9_NATIVE_RESOURCE_H
#include "../pw_d3d9_resource_wire.h"
#include "pw_d3d9_kinds.h"
struct pw_d3d9_native_resource;
/* Service-thread local API, externally serialized. Registry owns returned
 * contexts; it must retain the parent device/window until resource destruction.
 * The dispatcher supplies the service-assigned object ID in CREATE replies. */
void pw_d3d9_native_resource_create(void *native_device,
 const struct pw_d3d9_resource_request *,struct pw_d3d9_resource_reply *,
 struct pw_d3d9_native_resource **);
void pw_d3d9_native_resource_call(struct pw_d3d9_native_resource *,
 const struct pw_d3d9_resource_request *,struct pw_d3d9_resource_reply *);
uintptr_t pw_d3d9_native_resource_identity(struct pw_d3d9_native_resource *);
void *pw_d3d9_native_resource_backend(struct pw_d3d9_native_resource *);
uint32_t pw_d3d9_native_resource_kind(struct pw_d3d9_native_resource *);
/* Returns actual cleanup Unlock HRESULT, still releases all owned COM refs. */
uint32_t pw_d3d9_native_resource_destroy(struct pw_d3d9_native_resource *);
#endif
