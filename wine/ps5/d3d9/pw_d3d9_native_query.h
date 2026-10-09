/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_NATIVE_QUERY_H
#define PW_D3D9_NATIVE_QUERY_H
#include <d3d9.h>
#include "../pw_d3d9_query_wire.h"
struct pw_d3d9_native_query;
void pw_d3d9_native_query_create(IDirect3DDevice9 *,const struct pw_d3d9_query_request *,struct pw_d3d9_query_reply *,struct pw_d3d9_native_query **);
void pw_d3d9_native_query_call(struct pw_d3d9_native_query *,const struct pw_d3d9_query_request *,struct pw_d3d9_query_reply *);
/* Borrowed identity, kept alive by the context. Service owns registry IDs. */
IUnknown *pw_d3d9_native_query_identity(struct pw_d3d9_native_query *);
void pw_d3d9_native_query_destroy(struct pw_d3d9_native_query *);
#endif
