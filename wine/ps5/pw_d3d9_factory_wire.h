/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_FACTORY_WIRE_H
#define PW_D3D9_FACTORY_WIRE_H
#include <stddef.h>
#include <stdint.h>
#define PW_D3D9_FACTORY_VERSION 1u
#define PW_D3D9_FACTORY_HEADER 16u
#define PW_D3D9_FACTORY_MAX_REPLY 1116u
/* Caps fields are individual 32-bit values. F32 fields carry IEEE754 bits,
 * I32 fields carry two's-complement bits. Third macro argument names the
 * native D3DCAPS9 member for field-wise adapters; never memcpy a native caps. */
#define PW_D3D9_CAP_FIELDS(X) \
 X(U32,DeviceType,DeviceType) \
 X(U32,AdapterOrdinal,AdapterOrdinal) \
 X(U32,Caps,Caps) \
 X(U32,Caps2,Caps2) \
 X(U32,Caps3,Caps3) \
 X(U32,PresentationIntervals,PresentationIntervals) \
 X(U32,CursorCaps,CursorCaps) \
 X(U32,DevCaps,DevCaps) \
 X(U32,PrimitiveMiscCaps,PrimitiveMiscCaps) \
 X(U32,RasterCaps,RasterCaps) \
 X(U32,ZCmpCaps,ZCmpCaps) \
 X(U32,SrcBlendCaps,SrcBlendCaps) \
 X(U32,DestBlendCaps,DestBlendCaps) \
 X(U32,AlphaCmpCaps,AlphaCmpCaps) \
 X(U32,ShadeCaps,ShadeCaps) \
 X(U32,TextureCaps,TextureCaps) \
 X(U32,TextureFilterCaps,TextureFilterCaps) \
 X(U32,CubeTextureFilterCaps,CubeTextureFilterCaps) \
 X(U32,VolumeTextureFilterCaps,VolumeTextureFilterCaps) \
 X(U32,TextureAddressCaps,TextureAddressCaps) \
 X(U32,VolumeTextureAddressCaps,VolumeTextureAddressCaps) \
 X(U32,LineCaps,LineCaps) \
 X(U32,MaxTextureWidth,MaxTextureWidth) \
 X(U32,MaxTextureHeight,MaxTextureHeight) \
 X(U32,MaxVolumeExtent,MaxVolumeExtent) \
 X(U32,MaxTextureRepeat,MaxTextureRepeat) \
 X(U32,MaxTextureAspectRatio,MaxTextureAspectRatio) \
 X(U32,MaxAnisotropy,MaxAnisotropy) \
 X(F32,MaxVertexW,MaxVertexW) \
 X(F32,GuardBandLeft,GuardBandLeft) \
 X(F32,GuardBandTop,GuardBandTop) \
 X(F32,GuardBandRight,GuardBandRight) \
 X(F32,GuardBandBottom,GuardBandBottom) \
 X(F32,ExtentsAdjust,ExtentsAdjust) \
 X(U32,StencilCaps,StencilCaps) \
 X(U32,FVFCaps,FVFCaps) \
 X(U32,TextureOpCaps,TextureOpCaps) \
 X(U32,MaxTextureBlendStages,MaxTextureBlendStages) \
 X(U32,MaxSimultaneousTextures,MaxSimultaneousTextures) \
 X(U32,VertexProcessingCaps,VertexProcessingCaps) \
 X(U32,MaxActiveLights,MaxActiveLights) \
 X(U32,MaxUserClipPlanes,MaxUserClipPlanes) \
 X(U32,MaxVertexBlendMatrices,MaxVertexBlendMatrices) \
 X(U32,MaxVertexBlendMatrixIndex,MaxVertexBlendMatrixIndex) \
 X(F32,MaxPointSize,MaxPointSize) \
 X(U32,MaxPrimitiveCount,MaxPrimitiveCount) \
 X(U32,MaxVertexIndex,MaxVertexIndex) \
 X(U32,MaxStreams,MaxStreams) \
 X(U32,MaxStreamStride,MaxStreamStride) \
 X(U32,VertexShaderVersion,VertexShaderVersion) \
 X(U32,MaxVertexShaderConst,MaxVertexShaderConst) \
 X(U32,PixelShaderVersion,PixelShaderVersion) \
 X(F32,PixelShader1xMaxValue,PixelShader1xMaxValue) \
 X(U32,DevCaps2,DevCaps2) \
 X(F32,MaxNpatchTessellationLevel,MaxNpatchTessellationLevel) \
 X(U32,Reserved5,Reserved5) \
 X(U32,MasterAdapterOrdinal,MasterAdapterOrdinal) \
 X(U32,AdapterOrdinalInGroup,AdapterOrdinalInGroup) \
 X(U32,NumberOfAdaptersInGroup,NumberOfAdaptersInGroup) \
 X(U32,DeclTypes,DeclTypes) \
 X(U32,NumSimultaneousRTs,NumSimultaneousRTs) \
 X(U32,StretchRectFilterCaps,StretchRectFilterCaps) \
 X(U32,VS20Caps_Caps,VS20Caps.Caps) \
 X(I32,VS20Caps_DynamicFlowControlDepth,VS20Caps.DynamicFlowControlDepth) \
 X(I32,VS20Caps_NumTemps,VS20Caps.NumTemps) \
 X(I32,VS20Caps_StaticFlowControlDepth,VS20Caps.StaticFlowControlDepth) \
 X(U32,PS20Caps_Caps,PS20Caps.Caps) \
 X(I32,PS20Caps_DynamicFlowControlDepth,PS20Caps.DynamicFlowControlDepth) \
 X(I32,PS20Caps_NumTemps,PS20Caps.NumTemps) \
 X(I32,PS20Caps_StaticFlowControlDepth,PS20Caps.StaticFlowControlDepth) \
 X(I32,PS20Caps_NumInstructionSlots,PS20Caps.NumInstructionSlots) \
 X(U32,VertexTextureFilterCaps,VertexTextureFilterCaps) \
 X(U32,MaxVShaderInstructionsExecuted,MaxVShaderInstructionsExecuted) \
 X(U32,MaxPShaderInstructionsExecuted,MaxPShaderInstructionsExecuted) \
 X(U32,MaxVertexShader30InstructionSlots,MaxVertexShader30InstructionSlots) \
 X(U32,MaxPixelShader30InstructionSlots,MaxPixelShader30InstructionSlots)
#define PW_D3D9_CAP_WORDS 76u
struct pw_d3d9_caps {
#define PW_CAP_DECLARE(type,name,native) uint32_t name;
 PW_D3D9_CAP_FIELDS(PW_CAP_DECLARE)
#undef PW_CAP_DECLARE
};
struct pw_d3d9_adapter_identifier {
 char driver[512],description[512],device_name[32];
 uint64_t driver_version;
 uint32_t vendor_id,device_id,subsystem_id,revision;
 uint32_t guid_data1;uint16_t guid_data2,guid_data3;uint8_t guid_data4[8];
 uint32_t whql_level;
};
struct pw_d3d9_display_mode { uint32_t width,height,refresh_rate,format; };
/* Method numbers are IDirect3D9 vtable slots. Only fields belonging to that
 * method are encoded; decoders zero all other fields. Formats/enums reach the
 * real backend unchanged. BOOL windowed must be canonical zero or one. */
struct pw_d3d9_factory_request {
 uint32_t method,adapter,device_type,format,format2,format3,usage,resource_type;
 uint32_t mode,flags,windowed,multisample;
};
struct pw_d3d9_factory_reply {
 uint32_t method,hresult,count;
 struct pw_d3d9_adapter_identifier identifier;
 struct pw_d3d9_display_mode mode;
 struct pw_d3d9_caps caps;
};
enum pw_d3d9_factory_codec_result {
 PW_D3D9_FACTORY_OK, PW_D3D9_FACTORY_INVALID, PW_D3D9_FACTORY_SMALL,
 PW_D3D9_FACTORY_UNSUPPORTED
};
/* Slots3,15,16 are explicitly unsupported. No codec synthesizes success.
 * Failure HRESULTs carry no output. Count methods4/6 require HRESULT0.
 * Encoders report required size on SMALL and do not modify output on error.
 * Input/output objects must not overlap. DTOs are local, not wire layouts. */
int pw_d3d9_factory_request_encode(void *,size_t,size_t *,const struct pw_d3d9_factory_request *);
int pw_d3d9_factory_request_decode(struct pw_d3d9_factory_request *,const void *,size_t);
int pw_d3d9_factory_reply_encode(void *,size_t,size_t *,const struct pw_d3d9_factory_reply *);
int pw_d3d9_factory_reply_decode(struct pw_d3d9_factory_reply *,const void *,size_t);
#endif
