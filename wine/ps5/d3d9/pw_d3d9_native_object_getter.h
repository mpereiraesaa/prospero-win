/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_NATIVE_OBJECT_GETTER_H
#define PW_D3D9_NATIVE_OBJECT_GETTER_H
#include "../pw_d3d9_object_getter.h"
/* Service-only result. object owns exactly the backend getter's returned COM
 * reference; caller must release or transfer it into a registry-owned wrapper.
 * NULL is a real null binding. Never serialize this structure. */
struct pw_d3d9_native_object_result {void *object;uint32_t kind,offset,stride;};
uint32_t pw_d3d9_native_object_getter(void *,const struct pw_d3d9_object_getter_request *,struct pw_d3d9_native_object_result *);
void pw_d3d9_native_object_result_release(struct pw_d3d9_native_object_result *);
#endif
