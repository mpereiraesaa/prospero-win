/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_NATIVE_PROGRAM_H
#define PW_D3D9_NATIVE_PROGRAM_H
#include "../pw_d3d9_program_wire.h"
struct pw_d3d9_native_program;
/* Native service thread only; serialized by caller. Inputs are the owned,
 * validated canonical bytes from the upload assembler. Creation returns the
 * exact backend HRESULT. Registry publication is the dispatcher's responsibility. */
uint32_t pw_d3d9_native_program_create(void *,uint32_t,const void *,size_t,
 struct pw_d3d9_native_program **);
/* Consumes one owned backend reference on EVERY path; validates kind/device. */
uint32_t pw_d3d9_native_program_adopt(void *,uint32_t,void *,struct pw_d3d9_native_program **);
uintptr_t pw_d3d9_native_program_identity(struct pw_d3d9_native_program *);
void *pw_d3d9_native_program_backend(struct pw_d3d9_native_program *);
uint32_t pw_d3d9_native_program_kind(struct pw_d3d9_native_program *);
void pw_d3d9_native_program_destroy(struct pw_d3d9_native_program *);
#endif
