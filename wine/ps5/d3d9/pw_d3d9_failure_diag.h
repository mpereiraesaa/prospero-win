/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_FAILURE_DIAG_H
#define PW_D3D9_FAILURE_DIAG_H
#include <stdio.h>
#include <inttypes.h>
#include "../pw_d3d9_factory_wire.h"
#include "../pw_d3d9_texture_wire.h"
/* Failure-only service diagnostics. Log copied scalar inputs, never process
 * pointers or payload bytes. Successful calls produce no additional logging. */
static inline void pw_d3d9_factory_failure(FILE *stream,const struct pw_d3d9_factory_request *q,uint32_t hr)
{
 static const char *const names[]={"GetAdapterCount","GetAdapterIdentifier","GetAdapterModeCount","EnumAdapterModes","GetAdapterDisplayMode","CheckDeviceType","CheckDeviceFormat","CheckDeviceMultiSampleType","CheckDepthStencilMatch","CheckDeviceFormatConversion","GetDeviceCaps"};
 if(!(hr&UINT32_C(0x80000000)))return;
 const char *name=q->method>=4&&q->method<=14?names[q->method-4]:"unknown";
 fprintf(stream,"PW_D3D9 backend family=factory method=%"PRIu32" name=%s hr=%08"PRIx32" adapter=%"PRIu32" type=%"PRIu32" format=%"PRIu32" format2=%"PRIu32" format3=%"PRIu32" usage=%08"PRIx32" resource_type=%"PRIu32" mode=%"PRIu32" flags=%08"PRIx32" windowed=%"PRIu32" multisample=%"PRIu32"\n",q->method,name,hr,q->adapter,q->device_type,q->format,q->format2,q->format3,q->usage,q->resource_type,q->mode,q->flags,q->windowed,q->multisample);
}
static inline void pw_d3d9_texture_failure(FILE *stream,const struct pw_d3d9_texture_request *q,uint32_t hr)
{
 static const char *const names[]={"CreateTexture","CreateOffscreenPlainSurface","GetDesc","GetSurfaceLevel","LockRect","Read","Write","UnlockRect","CancelLock","AddDirtyRect","UpdateTexture","UpdateSurface","CreateRenderTarget","CreateDepthStencilSurface","StretchRect","ColorFill","GetRenderTargetData","GetPriority","SetPriority","PreLoad","GetLOD","SetLOD","GetAutoGenFilterType","SetAutoGenFilterType","GenerateMipSubLevels","GetContainer"};
 if(!(hr&UINT32_C(0x80000000)))return;
 const char *name=q->operation>=1&&q->operation<=26?names[q->operation-1]:"unknown";
 fprintf(stream,"PW_D3D9 backend family=texture operation=%"PRIu32" name=%s hr=%08"PRIx32" width=%"PRIu32" height=%"PRIu32" levels=%"PRIu32" level=%"PRIu32" format=%"PRIu32" usage=%08"PRIx32" pool=%"PRIu32" flags=%08"PRIx32" filter=%"PRIu32" value=%"PRIu32"\n",q->operation,name,hr,q->width,q->height,q->levels,q->level,q->format,q->usage,q->pool,q->flags,q->filter,q->value);
}
#endif
