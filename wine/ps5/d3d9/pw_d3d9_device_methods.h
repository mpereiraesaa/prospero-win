/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_DEVICE_METHODS_H
#define PW_D3D9_DEVICE_METHODS_H
#include <d3d9.h>
#include "../pw_d3d9_command_wire.h"
#include "../pw_d3d9_getter_wire.h"
#include "../pw_d3d9_objects.h"
#include "pw_d3d9_kinds.h"
struct pw_d3d9_device_methods_ops {
 HRESULT (*command)(IDirect3DDevice9 *,const struct pw_d3d9_command *);
 HRESULT (*getter)(IDirect3DDevice9 *,const struct pw_d3d9_getter_request *,struct pw_d3d9_getter_reply *);
 void (*fail)(IDirect3DDevice9 *,HRESULT);
 HRESULT (*resolve)(IDirect3DDevice9 *,IUnknown *,uint32_t,struct pw_d3d9_object_ref *);
};
/* Call once under the owning device-vtable initialization lock, before publication.
 * Callbacks synchronously pin self/session; resolve validates same-device proxies.
 * Only the 40 command and 28 scalar/structure getter slots are installed. */
void pw_d3d9_device_methods_install(IDirect3DDevice9Vtbl *,const struct pw_d3d9_device_methods_ops *);
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
/* The callback owns typed resolution and admission as one transaction. It must
 * pin the device through callbacks, validate the local object under its family
 * cache lock, and retain its private ticket through service acknowledgement. */
typedef HRESULT (*pw_d3d9_device_binding_fn)(IDirect3DDevice9 *,
 struct pw_d3d9_command *,IUnknown *,uint32_t,uint32_t);
void pw_d3d9_device_methods_binding_install(pw_d3d9_device_binding_fn);
#endif
#endif
