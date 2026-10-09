/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_DEVICE_OBJECT_METHODS_H
#define PW_D3D9_DEVICE_OBJECT_METHODS_H
#include <d3d9.h>
#include "../pw_d3d9_object_getter.h"
#include "../pw_d3d9_objects.h"
struct pw_d3d9_device_object_methods_ops {
 HRESULT (*getter)(IDirect3DDevice9 *,const struct pw_d3d9_object_getter_request *,struct pw_d3d9_object_getter_reply *);
 /* Consumes the returned owned remote reference on every path. */
 HRESULT (*wrap)(IDirect3DDevice9 *,uint32_t kind,struct pw_d3d9_object_ref,void **);
 /* Cancels/sticks malformed transport; service retires any acquired remote ref. */
 void (*fail)(IDirect3DDevice9 *,HRESULT);
};
/* Once before vtable publication. getter/wrap callbacks pin device/session. */
void pw_d3d9_device_object_methods_install(IDirect3DDevice9Vtbl *,const struct pw_d3d9_device_object_methods_ops *);
#endif
