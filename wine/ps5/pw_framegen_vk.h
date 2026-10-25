/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_FRAMEGEN_VK_H
#define PW_FRAMEGEN_VK_H
/* The Vulkan functions frame generation calls, as pointers: inside
 * libvulkan.prx there is no loader to resolve vkCreateImage and the like by
 * name, so this header, included before anything else in pw_framegen.cpp
 * and in MihawkGen's sources (third_party/mihawkgen), turns each name into
 * a pointer that pw_framegen_load fills from the driver's own lookups. */
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

/* Device functions: MihawkGen's, then the shim's own. */
#define PW_FRAMEGEN_VK_DEVICE(X) \
    X(AllocateDescriptorSets) X(AllocateMemory) X(BindBufferMemory) X(BindImageMemory) \
    X(CmdBindDescriptorSets) X(CmdBindPipeline) X(CmdClearColorImage) X(CmdDispatch) \
    X(CmdPipelineBarrier) X(CmdPushConstants) X(CmdWriteTimestamp) X(CreateBuffer) \
    X(CreateComputePipelines) X(CreateDescriptorPool) X(CreateDescriptorSetLayout) X(CreateImage) \
    X(CreateImageView) X(CreatePipelineLayout) X(CreateSampler) X(CreateShaderModule) \
    X(DestroyBuffer) X(DestroyDescriptorPool) X(DestroyDescriptorSetLayout) X(DestroyImage) \
    X(DestroyImageView) X(DestroyPipeline) X(DestroyPipelineLayout) X(DestroySampler) \
    X(DestroyShaderModule) X(FreeMemory) X(GetBufferMemoryRequirements) X(GetImageMemoryRequirements) \
    X(MapMemory) X(UnmapMemory) X(UpdateDescriptorSets) \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) \
    X(EndCommandBuffer) X(ResetCommandBuffer) X(QueueSubmit) X(CreateSemaphore) X(DestroySemaphore) \
    X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) X(CmdBlitImage) \
    X(CmdCopyBufferToImage) X(GetSwapchainImagesKHR) X(AcquireNextImageKHR) X(DeviceWaitIdle)
/* Instance functions. */
#define PW_FRAMEGEN_VK_INSTANCE(X) X(GetPhysicalDeviceMemoryProperties)

#ifdef __cplusplus
extern "C" {
#endif
#define PW_FRAMEGEN_VK_DECLARE(name) extern PFN_vk##name pw_fgvk_##name;
PW_FRAMEGEN_VK_DEVICE(PW_FRAMEGEN_VK_DECLARE)
PW_FRAMEGEN_VK_INSTANCE(PW_FRAMEGEN_VK_DECLARE)
#undef PW_FRAMEGEN_VK_DECLARE
#ifdef __cplusplus
}
#endif

#define vkAllocateDescriptorSets pw_fgvk_AllocateDescriptorSets
#define vkAllocateMemory pw_fgvk_AllocateMemory
#define vkBindBufferMemory pw_fgvk_BindBufferMemory
#define vkBindImageMemory pw_fgvk_BindImageMemory
#define vkCmdBindDescriptorSets pw_fgvk_CmdBindDescriptorSets
#define vkCmdBindPipeline pw_fgvk_CmdBindPipeline
#define vkCmdClearColorImage pw_fgvk_CmdClearColorImage
#define vkCmdDispatch pw_fgvk_CmdDispatch
#define vkCmdPipelineBarrier pw_fgvk_CmdPipelineBarrier
#define vkCmdPushConstants pw_fgvk_CmdPushConstants
#define vkCmdWriteTimestamp pw_fgvk_CmdWriteTimestamp
#define vkCreateBuffer pw_fgvk_CreateBuffer
#define vkCreateComputePipelines pw_fgvk_CreateComputePipelines
#define vkCreateDescriptorPool pw_fgvk_CreateDescriptorPool
#define vkCreateDescriptorSetLayout pw_fgvk_CreateDescriptorSetLayout
#define vkCreateImage pw_fgvk_CreateImage
#define vkCreateImageView pw_fgvk_CreateImageView
#define vkCreatePipelineLayout pw_fgvk_CreatePipelineLayout
#define vkCreateSampler pw_fgvk_CreateSampler
#define vkCreateShaderModule pw_fgvk_CreateShaderModule
#define vkDestroyBuffer pw_fgvk_DestroyBuffer
#define vkDestroyDescriptorPool pw_fgvk_DestroyDescriptorPool
#define vkDestroyDescriptorSetLayout pw_fgvk_DestroyDescriptorSetLayout
#define vkDestroyImage pw_fgvk_DestroyImage
#define vkDestroyImageView pw_fgvk_DestroyImageView
#define vkDestroyPipeline pw_fgvk_DestroyPipeline
#define vkDestroyPipelineLayout pw_fgvk_DestroyPipelineLayout
#define vkDestroySampler pw_fgvk_DestroySampler
#define vkDestroyShaderModule pw_fgvk_DestroyShaderModule
#define vkFreeMemory pw_fgvk_FreeMemory
#define vkGetBufferMemoryRequirements pw_fgvk_GetBufferMemoryRequirements
#define vkGetImageMemoryRequirements pw_fgvk_GetImageMemoryRequirements
#define vkMapMemory pw_fgvk_MapMemory
#define vkUnmapMemory pw_fgvk_UnmapMemory
#define vkUpdateDescriptorSets pw_fgvk_UpdateDescriptorSets
#define vkCreateCommandPool pw_fgvk_CreateCommandPool
#define vkDestroyCommandPool pw_fgvk_DestroyCommandPool
#define vkAllocateCommandBuffers pw_fgvk_AllocateCommandBuffers
#define vkBeginCommandBuffer pw_fgvk_BeginCommandBuffer
#define vkEndCommandBuffer pw_fgvk_EndCommandBuffer
#define vkResetCommandBuffer pw_fgvk_ResetCommandBuffer
#define vkQueueSubmit pw_fgvk_QueueSubmit
#define vkCreateSemaphore pw_fgvk_CreateSemaphore
#define vkDestroySemaphore pw_fgvk_DestroySemaphore
#define vkCreateFence pw_fgvk_CreateFence
#define vkDestroyFence pw_fgvk_DestroyFence
#define vkWaitForFences pw_fgvk_WaitForFences
#define vkResetFences pw_fgvk_ResetFences
#define vkCmdBlitImage pw_fgvk_CmdBlitImage
#define vkCmdCopyBufferToImage pw_fgvk_CmdCopyBufferToImage
#define vkGetSwapchainImagesKHR pw_fgvk_GetSwapchainImagesKHR
#define vkAcquireNextImageKHR pw_fgvk_AcquireNextImageKHR
#define vkDeviceWaitIdle pw_fgvk_DeviceWaitIdle
#define vkGetPhysicalDeviceMemoryProperties pw_fgvk_GetPhysicalDeviceMemoryProperties

#endif
