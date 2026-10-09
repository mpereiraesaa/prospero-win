/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_COMMAND_WIRE_H
#define PW_D3D9_COMMAND_WIRE_H
#include <stddef.h>
#include <stdint.h>
#define PW_D3D9_COMMAND_VERSION 1u
#define PW_D3D9_COMMAND_WORDS 8u
#define PW_D3D9_COMMAND_DATA 4096u
#define PW_D3D9_COMMAND_MAX (16u + 4u * PW_D3D9_COMMAND_WORDS + PW_D3D9_COMMAND_DATA)
enum pw_d3d9_command_shape {
    PW_D3D9_DATA_NONE, PW_D3D9_DATA_MATRIX, PW_D3D9_DATA_VIEWPORT,
    PW_D3D9_DATA_MATERIAL, PW_D3D9_DATA_LIGHT, PW_D3D9_DATA_PLANE,
    PW_D3D9_DATA_CLIP, PW_D3D9_DATA_RECT, PW_D3D9_DATA_PALETTE,
    PW_D3D9_DATA_VECTOR4, PW_D3D9_DATA_BOOL, PW_D3D9_DATA_CLEAR
};
/* X(slot, method, argument words, data shape, object-start mask, BOOL mask).
 * An object occupies two words: ID/generation. Null is exactly {0,0}.
 * All calls require synchronous exact HRESULT replies in the first adapter. */
#define PW_D3D9_COMMAND_METHODS(X) \
    X(3, TestCooperativeLevel, 0, NONE, 0, 0) \
    X(5, EvictManagedResources, 0, NONE, 0, 0) \
    X(20, SetDialogBoxMode, 1, NONE, 0, 1) \
    X(37, SetRenderTarget, 3, NONE, 2, 0) \
    X(39, SetDepthStencilSurface, 2, NONE, 1, 0) \
    X(41, BeginScene, 0, NONE, 0, 0) \
    X(42, EndScene, 0, NONE, 0, 0) \
    X(43, Clear, 5, CLEAR, 0, 0) \
    X(44, SetTransform, 1, MATRIX, 0, 0) \
    X(46, MultiplyTransform, 1, MATRIX, 0, 0) \
    X(47, SetViewport, 0, VIEWPORT, 0, 0) \
    X(49, SetMaterial, 0, MATERIAL, 0, 0) \
    X(51, SetLight, 1, LIGHT, 0, 0) \
    X(53, LightEnable, 2, NONE, 0, 2) \
    X(55, SetClipPlane, 1, PLANE, 0, 0) \
    X(57, SetRenderState, 2, NONE, 0, 0) \
    X(62, SetClipStatus, 0, CLIP, 0, 0) \
    X(65, SetTexture, 3, NONE, 2, 0) \
    X(67, SetTextureStageState, 3, NONE, 0, 0) \
    X(69, SetSamplerState, 3, NONE, 0, 0) \
    X(71, SetPaletteEntries, 1, PALETTE, 0, 0) \
    X(73, SetCurrentTexturePalette, 1, NONE, 0, 0) \
    X(75, SetScissorRect, 0, RECT, 0, 0) \
    X(77, SetSoftwareVertexProcessing, 1, NONE, 0, 1) \
    X(79, SetNPatchMode, 1, NONE, 0, 0) \
    X(81, DrawPrimitive, 3, NONE, 0, 0) \
    X(82, DrawIndexedPrimitive, 6, NONE, 0, 0) \
    X(87, SetVertexDeclaration, 2, NONE, 1, 0) \
    X(89, SetFVF, 1, NONE, 0, 0) \
    X(92, SetVertexShader, 2, NONE, 1, 0) \
    X(94, SetVertexShaderConstantF, 2, VECTOR4, 0, 0) \
    X(96, SetVertexShaderConstantI, 2, VECTOR4, 0, 0) \
    X(98, SetVertexShaderConstantB, 2, BOOL, 0, 0) \
    X(100, SetStreamSource, 5, NONE, 2, 0) \
    X(102, SetStreamSourceFreq, 2, NONE, 0, 0) \
    X(104, SetIndices, 2, NONE, 1, 0) \
    X(107, SetPixelShader, 2, NONE, 1, 0) \
    X(109, SetPixelShaderConstantF, 2, VECTOR4, 0, 0) \
    X(111, SetPixelShaderConstantI, 2, VECTOR4, 0, 0) \
    X(113, SetPixelShaderConstantB, 2, BOOL, 0, 0)
struct pw_d3d9_command_schema {
    uint32_t method, words, shape, object_words, boolean_words;
};
struct pw_d3d9_command {
    uint32_t method, data_bytes, args[PW_D3D9_COMMAND_WORDS];
    union { uint32_t words[PW_D3D9_COMMAND_DATA / 4]; unsigned char bytes[PW_D3D9_COMMAND_DATA]; } data;
};
enum pw_d3d9_command_result {
    PW_D3D9_COMMAND_OK, PW_D3D9_COMMAND_INVALID, PW_D3D9_COMMAND_SMALL,
    PW_D3D9_COMMAND_UNSUPPORTED
};
const struct pw_d3d9_command_schema *pw_d3d9_command_schema(uint32_t);
/* Compute exact required owned bytes without reading caller data. This rejects
 * arithmetic overflow and oversized arrays before copying. UP draws remain
 * unsupported until vertex-declaration-dependent copy ranges are modeled. */
int pw_d3d9_command_data_bytes(uint32_t, const uint32_t [PW_D3D9_COMMAND_WORDS], size_t *);
int pw_d3d9_command_encode(void *, size_t, size_t *, const struct pw_d3d9_command *);
/* Same wire validation/status as decode with a valid output pointer, without
 * materializing a command. Does not own input or authorize later execution. */
int pw_d3d9_command_validate(const void *, size_t);
int pw_d3d9_command_decode(struct pw_d3d9_command *, const void *, size_t);
int pw_d3d9_command_reply_encode(void *, size_t, size_t *, uint32_t method, uint32_t hresult);
int pw_d3d9_command_reply_decode(uint32_t *method, uint32_t *hresult, const void *, size_t);
#endif
