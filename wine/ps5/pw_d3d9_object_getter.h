/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_OBJECT_GETTER_H
#define PW_D3D9_OBJECT_GETTER_H
#include <stddef.h>
#include <stdint.h>
/* X(slot,name,argument_words,known_kind,extra_reply_words). Texture kind5 means
 * 2D; other base texture types need explicit future resource-kind support. */
#define PW_D3D9_OBJECT_GETTERS(X) \
 X(18,GetBackBuffer,3,6,0) \
 X(38,GetRenderTarget,1,6,0) \
 X(40,GetDepthStencilSurface,0,6,0) \
 X(64,GetTexture,1,5,0) \
 X(88,GetVertexDeclaration,0,7,0) \
 X(93,GetVertexShader,0,8,0) \
 X(101,GetStreamSource,1,3,2) \
 X(105,GetIndices,0,4,0) \
 X(108,GetPixelShader,0,9,0)
struct pw_d3d9_object_getter_schema {uint32_t method,args,kind,extra;};
struct pw_d3d9_object_getter_request {uint32_t method,args[3];};
/* IDs belong to outer session and returned object's generation. Null is
 * exactly kind/id/generation all zero; stream offset/stride remain actual. */
struct pw_d3d9_object_getter_reply {uint32_t method,hresult,kind,id,generation,offset,stride;};
enum pw_d3d9_object_getter_result {PW_D3D9_OBJECT_GETTER_OK,PW_D3D9_OBJECT_GETTER_INVALID,PW_D3D9_OBJECT_GETTER_SMALL,PW_D3D9_OBJECT_GETTER_UNSUPPORTED};
const struct pw_d3d9_object_getter_schema *pw_d3d9_object_getter_schema(uint32_t);
int pw_d3d9_object_getter_encode(void *,size_t,size_t *,const struct pw_d3d9_object_getter_request *);
int pw_d3d9_object_getter_decode(struct pw_d3d9_object_getter_request *,const void *,size_t);
int pw_d3d9_object_getter_reply_encode(void *,size_t,size_t *,const struct pw_d3d9_object_getter_request *,const struct pw_d3d9_object_getter_reply *);
int pw_d3d9_object_getter_reply_decode(struct pw_d3d9_object_getter_reply *,const struct pw_d3d9_object_getter_request *,const void *,size_t);
#endif
