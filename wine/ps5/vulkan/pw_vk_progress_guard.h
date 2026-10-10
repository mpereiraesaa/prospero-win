/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Include after vulkan_loader.h. Driver waits and queue progress must not own
 * the process stream gate. Caller still owns Vulkan external synchronization.
 */
#ifndef PW_VK_PROGRESS_GUARD_H
#define PW_VK_PROGRESS_GUARD_H
static BOOL pw_vk_stream_progress_call(unsigned int code)
{
    switch (code)
    {
        case unix_vkWaitForFences:
        case unix_vkWaitSemaphores:
        case unix_vkWaitSemaphoresKHR:
        case unix_vkQueueWaitIdle:
        case unix_vkDeviceWaitIdle:
        case unix_vkAcquireNextImageKHR:
        case unix_vkAcquireNextImage2KHR:
        case unix_vkWaitForPresentKHR:
        case unix_vkWaitForPresent2KHR:
        /* Conservatively include polling/no-WAIT forms of these APIs too. */
        case unix_vkGetQueryPoolResults:
        case unix_vkAcquireProfilingLockKHR:
        case unix_vkDeferredOperationJoinKHR:
        case unix_vkLatencySleepNV:
        case unix_vkLatencySleepLegacyNV:
        case unix_vkQueueSubmit:
        case unix_vkQueueSubmit2:
        case unix_vkQueueSubmit2KHR:
        case unix_vkQueueBindSparse:
        case unix_vkQueuePresentKHR:
        case unix_vkSignalSemaphore:
        case unix_vkSignalSemaphoreKHR:
            return TRUE;
        default:
            return FALSE;
    }
}
/* Pipeline and shader-object creation runs the driver's compiler for tens of
 * milliseconds per call and references only objects already created
 * synchronously, so it needs no ordering against pending records either: the
 * stream drains under the gate, then the compile runs outside it. Under the
 * gate, DXVK's compile workers serialised every other Vulkan-calling thread
 * behind their compiles (GTA IV cold cache: 300-500 ms frames). */
static BOOL pw_vk_stream_compile_call(unsigned int code)
{
    switch (code)
    {
        case unix_vkCreateGraphicsPipelines:
        case unix_vkCreateComputePipelines:
        case unix_vkCreateRayTracingPipelinesKHR:
        case unix_vkCreateShadersEXT:
            return TRUE;
        default:
            return FALSE;
    }
}
#endif
