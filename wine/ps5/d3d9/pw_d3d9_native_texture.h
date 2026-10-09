/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_NATIVE_TEXTURE_H
#define PW_D3D9_NATIVE_TEXTURE_H
#include "../pw_d3d9_texture_wire.h"
#include "pw_d3d9_kinds.h"
struct pw_d3d9_native_texture;
/* Consumes the owned native COM reference on every path. Validates kind through
 * QueryInterface and checks GetDevice ownership. Caller retains parent window. */
uint32_t pw_d3d9_native_texture_adopt(void *native_device,uint32_t kind,void *owned_native_object,struct pw_d3d9_native_texture **);
void pw_d3d9_native_texture_create(void *,const struct pw_d3d9_texture_request *,struct pw_d3d9_texture_reply *,struct pw_d3d9_native_texture **);
/* SURFACE_LEVEL returns a new owned context. Registry deduplicates its canonical
 * IUnknown identity, balances extra backend refs, and assigns the reply ID. */
void pw_d3d9_native_texture_call(struct pw_d3d9_native_texture *,const struct pw_d3d9_texture_request *,struct pw_d3d9_texture_reply *,struct pw_d3d9_native_texture **);
void pw_d3d9_native_texture_copy(void *,struct pw_d3d9_native_texture *,struct pw_d3d9_native_texture *,const struct pw_d3d9_texture_request *,struct pw_d3d9_texture_reply *);
/* Calls actual Surface::GetContainer with requested bounded IID, returning one
 * owned typed Device9/Texture9 reference only after actual parent validation.
 * Session must publish canonical IDs and retain device/window ownership. */
void pw_d3d9_native_texture_container(struct pw_d3d9_native_texture *,const struct pw_d3d9_texture_request *,struct pw_d3d9_texture_reply *,void **owned);
uintptr_t pw_d3d9_native_texture_identity(struct pw_d3d9_native_texture *);
void *pw_d3d9_native_texture_backend(struct pw_d3d9_native_texture *);
uint32_t pw_d3d9_native_texture_kind(struct pw_d3d9_native_texture *);
uint32_t pw_d3d9_native_texture_destroy(struct pw_d3d9_native_texture *);
#endif
