/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Include after vulkan_loader.h in the PE adapter translation unit.
 * These guards only decide whether to disable; caller sets sticky disabled
 * under stream ordering gate BEFORE drain/original dispatch. */
#ifndef PW_VK_DISABLE_GUARD_H
#define PW_VK_DISABLE_GUARD_H
static BOOL pw_vk_stream_environment_unsafe(void)
{
    static const char * const names[] = {
        "VK_INSTANCE_LAYERS", "VK_LOADER_LAYERS_ENABLE", "VK_LAYER_PATH",
        "VK_ADD_LAYER_PATH", "VK_LAYER_SETTINGS_PATH"
    };
    unsigned int i;
    char value;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (GetEnvironmentVariableA(names[i], &value, sizeof(value))) return TRUE;
    return FALSE;
}
static BOOL pw_vk_stream_call_unsafe(unsigned int code, const void *args)
{
    switch (code)
    {
        case unix_vkCreateInstance:
        {
            const struct vkCreateInstance_params *p = args;
            const VkInstanceCreateInfo *c = p->pCreateInfo;
            /* Unknown chains are synchronous: includes create-time callbacks
             * and validation features/flags, without traversing unknown data. */
            return !c || c->enabledLayerCount || c->pNext || p->pAllocator;
        }
        case unix_vkCreateDevice:
        {
            const struct vkCreateDevice_params *p = args;
            const VkDeviceCreateInfo *c = p->pCreateInfo;
            /* Device pNext commonly contains ordinary feature enable chains. */
            return !c || c->enabledLayerCount || p->pAllocator;
        }
        case unix_vkCreateDebugUtilsMessengerEXT:
        case unix_vkCreateDebugReportCallbackEXT:
            /* Sticky even if subsequent creation fails. */
            return TRUE;
        default:
            return FALSE;
    }
}
#endif
