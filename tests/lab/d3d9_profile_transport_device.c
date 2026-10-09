/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* TEST ONLY: extend the lifetime fixture's ordinary HWND adapter with the
 * actual null-rectangle Present path exercised by the profiling smoke. */
#define pw_d3d9_native_device_call lifetime_device_call
#include "d3d9_surface_lifetime_device.c"
#undef pw_d3d9_native_device_call
void pw_d3d9_native_device_call(void *factory,struct pw_d3d9_native_device *device,
 const struct pw_d3d9_device_request *q,struct pw_d3d9_device_reply *r,struct pw_d3d9_native_device **created)
{
 if(q->operation!=PW_D3D9_DEVICE_PRESENT){lifetime_device_call(factory,device,q,r,created);return;}
 *created=NULL;*r=(struct pw_d3d9_device_reply){.operation=q->operation,.hresult=D3DERR_INVALIDCALL};
 if(device&&!q->present_fields&&!q->dirty_count&&!q->override_window.id&&!q->override_window.epoch&&!q->override_window.generation)
  r->hresult=IDirect3DDevice9_Present(device->device,NULL,NULL,NULL,NULL);
}
