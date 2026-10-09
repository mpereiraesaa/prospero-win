/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_API_OBSERVE_H
#define PW_D3D9_API_OBSERVE_H
#include <windows.h>
#include <d3d9.h>
/* Opt-in once per process. Disabled publication returns the original table.
 * An enabled interface accepts exactly one immutable source table; a different
 * table returns NULL. Install after all typed slots have been populated. */
int pw_d3d9_api_observe_enabled(void);
/* Identity-safe lookup; never calls COM or dereferences the supplied table. */
const void *pw_d3d9_api_original_vtable(const void *);
void pw_d3d9_api_failure(const char *,unsigned,const char *,HRESULT,void *,REFIID);
#include "pw_d3d9_api_observe_generated.h"
#ifdef PW_D3D9_ENABLE_API_OBSERVE
#define PW_D3D9_API_OBSERVE(type,table) ((type##Vtbl *)pw_d3d9_api_observe_##type(table))
#else
#define PW_D3D9_API_OBSERVE(type,table) (table)
#endif
#endif
