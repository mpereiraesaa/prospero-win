/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_SERVICE_METHODS_H
#define PW_D3D9_SERVICE_METHODS_H
#include <windows.h>
#include "../pw_d3d9_objects.h"
/* Serialized service call. Zero means malformed typed request/reply. Missing
 * targets are ordinary outer HRESULT failures with zero payload. */
int pw_d3d9_service_methods(struct pw_d3d9_objects *,struct pw_d3d9_object_ref,
 uint32_t,const void *,size_t,void *,size_t,size_t *,HRESULT *);
#endif
