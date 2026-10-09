/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_NATIVE_PROGRAM_QUERY_H
#define PW_D3D9_NATIVE_PROGRAM_QUERY_H
#include "pw_d3d9_native_program.h"
#include "../pw_d3d9_program_query.h"
/* Service thread only. Caller pins typed registry object for the entire call. */
void pw_d3d9_native_program_query(struct pw_d3d9_native_program *,const struct pw_d3d9_program_query_request *,struct pw_d3d9_program_query_reply *);
#endif
