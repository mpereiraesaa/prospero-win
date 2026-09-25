/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Software-composed frames through ps5-vulkan WSI: flush the mapped staging
 * buffer, acquire an image, PRESENT_SRC -> TRANSFER_DST, copy,
 * TRANSFER_DST -> PRESENT_SRC, submit and wait, then a synchronous present.
 * The sequence is the one tests/test_wsi_buffer_present.c pins in ps5-vulkan. */
#include "pw_present_vk_ps5.h"
#include <string.h>

enum { STRIDE=PW_PRESENT_VK_WIDTH*4 };
#define STAGING_BYTES ((VkDeviceSize)STRIDE*PW_PRESENT_VK_HEIGHT)
#define WAIT_NS UINT64_C(2000000000)
extern int sceSystemServiceHideSplashScreen(void);

static int fail(PwPresentVkPs5 *vk,const char *call,VkResult result)
{
    if(!vk->failed_call){vk->failed_call=call;vk->failed_result=(int32_t)result;}
    return result==VK_TIMEOUT?PW_ERR_LIMIT:result==VK_ERROR_OUT_OF_HOST_MEMORY ||
        result==VK_ERROR_OUT_OF_DEVICE_MEMORY?PW_ERR_VM:PW_ERR_STATE;
}
#define TRY(vk,call) do { VkResult r_=(call); if(r_!=VK_SUCCESS)return fail((vk),#call,r_); } while(0)

static VkImageMemoryBarrier barrier(VkImage image,VkImageLayout from,VkImageLayout to,
                                    VkAccessFlags src,VkAccessFlags dst)
{
    return (VkImageMemoryBarrier){.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=src,.dstAccessMask=dst,.oldLayout=from,.newLayout=to,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
}
static int wait_and_reset(PwPresentVkPs5 *vk,const char *what)
{
    VkResult result=vkWaitForFences(vk->device,1,&vk->fence,VK_TRUE,WAIT_NS);
    if(result!=VK_SUCCESS)return fail(vk,what,result);
    TRY(vk,vkResetFences(vk->device,1,&vk->fence));
    return PW_OK;
}
static int acquire(void *context,PwPresentTarget *target)
{
    PwPresentVkPs5 *vk=context;
    if(!vk || !vk->mapped || vk->lent || !target)return PW_ERR_PRECONDITION;
    *target=(PwPresentTarget){vk->mapped,PW_PRESENT_VK_WIDTH,PW_PRESENT_VK_HEIGHT,STRIDE,
        STAGING_BYTES};
    vk->lent=1;return PW_OK;
}
static int submit(void *context,uint64_t sequence,int commit)
{
    PwPresentVkPs5 *vk=context;
    if(!vk || !vk->lent)return PW_ERR_PRECONDITION;
    vk->lent=0;
    if(!commit)return PW_OK;
    VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory=vk->staging_memory,.offset=0,.size=VK_WHOLE_SIZE};
    TRY(vk,vkFlushMappedMemoryRanges(vk->device,1,&range));
    uint32_t slot=UINT32_MAX;
    TRY(vk,vkAcquireNextImageKHR(vk->device,vk->swapchain,WAIT_NS,VK_NULL_HANDLE,vk->fence,&slot));
    int status=wait_and_reset(vk,"acquire-fence");if(status!=PW_OK)return status;
    if(slot>=2)return fail(vk,"acquire-slot",VK_ERROR_UNKNOWN);
    TRY(vk,vkResetCommandBuffer(vk->commands,0));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    TRY(vk,vkBeginCommandBuffer(vk->commands,&begin));
    VkImageMemoryBarrier import=barrier(vk->images[slot],VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(vk->commands,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&import);
    VkBufferImageCopy whole={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .imageExtent={PW_PRESENT_VK_WIDTH,PW_PRESENT_VK_HEIGHT,1}};
    vkCmdCopyBufferToImage(vk->commands,vk->staging,vk->images[slot],
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&whole);
    VkImageMemoryBarrier release=barrier(vk->images[slot],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_ACCESS_TRANSFER_WRITE_BIT,0);
    vkCmdPipelineBarrier(vk->commands,VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,0,NULL,1,&release);
    TRY(vk,vkEndCommandBuffer(vk->commands));
    VkSubmitInfo work={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,
        .pCommandBuffers=&vk->commands};
    TRY(vk,vkQueueSubmit(vk->queue,1,&work,vk->fence));
    vk->submits++;
    if((status=wait_and_reset(vk,"copy-fence"))!=PW_OK)return status;
    VkResult presented=VK_ERROR_UNKNOWN;
    VkPresentInfoKHR info={.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,.swapchainCount=1,
        .pSwapchains=&vk->swapchain,.pImageIndices=&slot,.pResults=&presented};
    TRY(vk,vkQueuePresentKHR(vk->queue,&info));
    if(presented!=VK_SUCCESS)return fail(vk,"present-result",presented);
    vk->flips++;vk->last_slot=slot;vk->last_sequence=sequence;
    return PW_OK;
}
static int create_surface(PwPresentVkPs5 *vk)
{
    uint32_t count=1;
    TRY(vk,vkEnumeratePhysicalDevices(vk->instance,&count,&vk->physical));
    if(count!=1 || !vk->physical)return fail(vk,"physical-device",VK_ERROR_INITIALIZATION_FAILED);
    VkDisplayPropertiesKHR display;memset(&display,0,sizeof(display));count=1;
    TRY(vk,vkGetPhysicalDeviceDisplayPropertiesKHR(vk->physical,&count,&display));
    VkDisplayModePropertiesKHR mode;memset(&mode,0,sizeof(mode));count=1;
    TRY(vk,vkGetDisplayModePropertiesKHR(vk->physical,display.display,&count,&mode));
    VkDisplaySurfaceCreateInfoKHR info={.sType=VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
        .displayMode=mode.displayMode,.planeIndex=0,.planeStackIndex=0,
        .transform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,.globalAlpha=1.0f,
        .alphaMode=VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
        .imageExtent={PW_PRESENT_VK_WIDTH,PW_PRESENT_VK_HEIGHT}};
    TRY(vk,vkCreateDisplayPlaneSurfaceKHR(vk->instance,&info,NULL,&vk->surface));
    VkBool32 supported=VK_FALSE;
    TRY(vk,vkGetPhysicalDeviceSurfaceSupportKHR(vk->physical,0,vk->surface,&supported));
    if(!supported)return fail(vk,"surface-support",VK_ERROR_EXTENSION_NOT_PRESENT);
    return PW_OK;
}
static int create_device(PwPresentVkPs5 *vk)
{
    float priority=1.0f;
    VkDeviceQueueCreateInfo queue={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex=0,.queueCount=1,.pQueuePriorities=&priority};
    const char *extension=VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo info={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount=1,.pQueueCreateInfos=&queue,
        .enabledExtensionCount=1,.ppEnabledExtensionNames=&extension};
    TRY(vk,vkCreateDevice(vk->physical,&info,NULL,&vk->device));
    vkGetDeviceQueue(vk->device,0,0,&vk->queue);
    if(!vk->queue)return fail(vk,"device-queue",VK_ERROR_INITIALIZATION_FAILED);
    VkSwapchainCreateInfoKHR chain={.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface=vk->surface,.minImageCount=2,.imageFormat=VK_FORMAT_B8G8R8A8_UNORM,
        .imageColorSpace=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent={PW_PRESENT_VK_WIDTH,PW_PRESENT_VK_HEIGHT},.imageArrayLayers=1,
        .imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT,.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE,
        .preTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode=VK_PRESENT_MODE_FIFO_KHR,.clipped=VK_TRUE};
    TRY(vk,vkCreateSwapchainKHR(vk->device,&chain,NULL,&vk->swapchain));
    uint32_t count=2;
    TRY(vk,vkGetSwapchainImagesKHR(vk->device,vk->swapchain,&count,vk->images));
    if(count!=2)return fail(vk,"swapchain-images",VK_ERROR_INITIALIZATION_FAILED);
    return PW_OK;
}
static int create_resources(PwPresentVkPs5 *vk)
{
    VkCommandPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=0};
    TRY(vk,vkCreateCommandPool(vk->device,&pool,NULL,&vk->pool));
    VkCommandBufferAllocateInfo allocate={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=vk->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    TRY(vk,vkAllocateCommandBuffers(vk->device,&allocate,&vk->commands));
    VkFenceCreateInfo fence={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    TRY(vk,vkCreateFence(vk->device,&fence,NULL,&vk->fence));
    VkBufferCreateInfo buffer={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=STAGING_BYTES,
        .usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    TRY(vk,vkCreateBuffer(vk->device,&buffer,NULL,&vk->staging));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(vk->device,vk->staging,&requirements);
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(vk->physical,&memory);
    uint32_t type=UINT32_MAX;
    for(uint32_t i=0;i<memory.memoryTypeCount && type==UINT32_MAX;i++)
        if((requirements.memoryTypeBits&(1u<<i)) &&
           (memory.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))type=i;
    if(type==UINT32_MAX)return fail(vk,"staging-memory-type",VK_ERROR_FEATURE_NOT_PRESENT);
    VkMemoryAllocateInfo info={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=requirements.size,.memoryTypeIndex=type};
    TRY(vk,vkAllocateMemory(vk->device,&info,NULL,&vk->staging_memory));
    TRY(vk,vkBindBufferMemory(vk->device,vk->staging,vk->staging_memory,0));
    void *mapped=NULL;
    TRY(vk,vkMapMemory(vk->device,vk->staging_memory,0,VK_WHOLE_SIZE,0,&mapped));
    vk->mapped=mapped;
    return PW_OK;
}
int pw_present_vk_ps5_open(PwPresentVkPs5 *vk,PwPresentSink *sink)
{
    if(!vk || !sink)return PW_ERR_PRECONDITION;
    memset(vk,0,sizeof(*vk));
    const char *extensions[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_DISPLAY_EXTENSION_NAME};
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName="prospero-win",.apiVersion=VK_API_VERSION_1_0};
    VkInstanceCreateInfo info={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo=&app,.enabledExtensionCount=2,.ppEnabledExtensionNames=extensions};
    VkResult result=vkCreateInstance(&info,NULL,&vk->instance);
    int status=result==VK_SUCCESS?PW_OK:fail(vk,"vkCreateInstance",result);
    if(status==PW_OK)status=create_surface(vk);
    if(status==PW_OK)status=create_device(vk);
    if(status==PW_OK)status=create_resources(vk);
    if(status!=PW_OK) {
        const char *call=vk->failed_call;int32_t rc=vk->failed_result;
        (void)pw_present_vk_ps5_close(vk);
        vk->failed_call=call;vk->failed_result=rc;
        return status;
    }
    (void)sceSystemServiceHideSplashScreen();
    *sink=(PwPresentSink){vk,PW_PRESENT_VK_WIDTH,PW_PRESENT_VK_HEIGHT,acquire,submit};
    return PW_OK;
}
int pw_present_vk_ps5_close(PwPresentVkPs5 *vk)
{
    if(!vk)return PW_ERR_PRECONDITION;
    int status=PW_OK;
    if(vk->device) {
        VkResult idle=vkDeviceWaitIdle(vk->device);
        if(idle!=VK_SUCCESS)status=fail(vk,"vkDeviceWaitIdle",idle);
        if(vk->mapped)vkUnmapMemory(vk->device,vk->staging_memory);
        if(vk->staging)vkDestroyBuffer(vk->device,vk->staging,NULL);
        if(vk->staging_memory)vkFreeMemory(vk->device,vk->staging_memory,NULL);
        if(vk->fence)vkDestroyFence(vk->device,vk->fence,NULL);
        if(vk->pool)vkDestroyCommandPool(vk->device,vk->pool,NULL);
        if(vk->swapchain)vkDestroySwapchainKHR(vk->device,vk->swapchain,NULL);
        vkDestroyDevice(vk->device,NULL);
    }
    if(vk->surface)vkDestroySurfaceKHR(vk->instance,vk->surface,NULL);
    if(vk->instance)vkDestroyInstance(vk->instance,NULL);
    const char *call=vk->failed_call;int32_t rc=vk->failed_result;
    uint64_t flips=vk->flips,submits=vk->submits;
    memset(vk,0,sizeof(*vk));
    vk->failed_call=call;vk->failed_result=rc;vk->flips=flips;vk->submits=submits;
    return status;
}
