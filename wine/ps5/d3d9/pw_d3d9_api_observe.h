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
enum pw_d3d9_api_field_kind { PW_D3D9_API_WORDS, PW_D3D9_API_POINTER, PW_D3D9_API_LOCKED_RECT };
struct pw_d3d9_api_field {
 const char *name;const void *address;SIZE_T bytes;enum pw_d3d9_api_field_kind kind;
};
int pw_d3d9_api_sample(LONG *);
int pw_d3d9_api_read(void *,const void *,SIZE_T);
void pw_d3d9_api_output(const char *,unsigned,const char *,HRESULT,void *,
 const struct pw_d3d9_api_field *,unsigned);
#include "pw_d3d9_api_observe_generated.h"
#ifdef PW_D3D9_ENABLE_API_OBSERVE
#define PW_D3D9_API_OBSERVE(type,table) ((type##Vtbl *)pw_d3d9_api_observe_##type(table))
#else
#define PW_D3D9_API_OBSERVE(type,table) (table)
#endif
#endif
