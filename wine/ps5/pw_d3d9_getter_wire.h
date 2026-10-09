/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_GETTER_WIRE_H
#define PW_D3D9_GETTER_WIRE_H
#include <stddef.h>
#include <stdint.h>
#define PW_D3D9_GETTER_DATA 4096u
#define PW_D3D9_GETTER_MAX (16u+PW_D3D9_GETTER_DATA)
/* X(slot,name,argument_words,fixed_output_bytes,array_element_bytes).
 * An array uses args[0]=start register, args[1]=element count. Zero elements are
 * represented exactly; the backend decides validity. Data is native-valued
 * DWORDs except the 256 explicit red/green/blue/flags palette entries. */
#define PW_D3D9_GETTER_METHODS(X) \
 X(4,GetAvailableTextureMem,0,4,0) \
 X(8,GetDisplayMode,1,16,0) \
 X(15,GetNumberOfSwapChains,0,4,0) \
 X(19,GetRasterStatus,1,8,0) \
 X(45,GetTransform,1,64,0) \
 X(48,GetViewport,0,24,0) \
 X(50,GetMaterial,0,68,0) \
 X(52,GetLight,1,104,0) \
 X(54,GetLightEnable,1,4,0) \
 X(56,GetClipPlane,1,16,0) \
 X(58,GetRenderState,1,4,0) \
 X(63,GetClipStatus,0,8,0) \
 X(66,GetTextureStageState,2,4,0) \
 X(68,GetSamplerState,2,4,0) \
 X(70,ValidateDevice,0,4,0) \
 X(72,GetPaletteEntries,1,1024,0) \
 X(74,GetCurrentTexturePalette,0,4,0) \
 X(76,GetScissorRect,0,16,0) \
 X(78,GetSoftwareVertexProcessing,0,4,0) \
 X(80,GetNPatchMode,0,4,0) \
 X(90,GetFVF,0,4,0) \
 X(95,GetVertexShaderConstantF,2,0,16) \
 X(97,GetVertexShaderConstantI,2,0,16) \
 X(99,GetVertexShaderConstantB,2,0,4) \
 X(103,GetStreamSourceFreq,1,4,0) \
 X(110,GetPixelShaderConstantF,2,0,16) \
 X(112,GetPixelShaderConstantI,2,0,16) \
 X(114,GetPixelShaderConstantB,2,0,4)
struct pw_d3d9_getter_schema {uint32_t method,args,bytes,element;};
struct pw_d3d9_getter_request {uint32_t method,args[2];};
struct pw_d3d9_getter_reply {
 uint32_t method,hresult,bytes;
 union {uint32_t words[PW_D3D9_GETTER_DATA/4];unsigned char bytes[PW_D3D9_GETTER_DATA];} data;
};
enum pw_d3d9_getter_result {PW_D3D9_GETTER_OK,PW_D3D9_GETTER_INVALID,PW_D3D9_GETTER_SMALL,PW_D3D9_GETTER_UNSUPPORTED};
const struct pw_d3d9_getter_schema *pw_d3d9_getter_schema(uint32_t);
int pw_d3d9_getter_bytes(const struct pw_d3d9_getter_request *,size_t *);
int pw_d3d9_getter_encode(void *,size_t,size_t *,const struct pw_d3d9_getter_request *);
int pw_d3d9_getter_decode(struct pw_d3d9_getter_request *,const void *,size_t);
/* Replies are checked against the original request before publication. Failed
 * HRESULT replies contain no output data. All value-return methods use S_OK
 * plus their exact returned UINT/BOOL/float bits, never a fabricated value. */
int pw_d3d9_getter_reply_encode(void *,size_t,size_t *,const struct pw_d3d9_getter_request *,const struct pw_d3d9_getter_reply *);
int pw_d3d9_getter_reply_decode(struct pw_d3d9_getter_reply *,const struct pw_d3d9_getter_request *,const void *,size_t);
#endif
