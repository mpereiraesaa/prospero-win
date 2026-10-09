#!/usr/bin/env python3
"""Stage bounded native command-pool fanout after lifecycle and barrier staging."""
import argparse
import ast
import inspect
import re
from pathlib import Path


def once(text, old, new):
    assert text.count(old) == 1, (old, text.count(old))
    return text.replace(old, new)


def fanout_call(name, prefix, conv):
    if name not in ('vkResetCommandPool', 'vkTrimCommandPool', 'vkTrimCommandPoolKHR'):
        return None
    device = prefix + 'device'
    if conv:
        device = '(VkDevice)UlongToPtr(' + device + ')'
    call = 'wine_' + name + '(' + device + ', ' + prefix + 'commandPool, ' + prefix + 'flags);\n'
    return '    ' + (prefix + 'result = ' if name == 'vkResetCommandPool' else '') + call


HELPERS = '''
/* One line per lifecycle event; a logical pool creates at most eight slots. */
static void pw_vk_pool_report(const struct wine_cmd_pool *pool, const char *event, VkResult result)
{
    const char *stats = getenv("PW_VK_BATCH_STATS"), *requested = getenv("PW_VK_REPLAY_POOL_FANOUT");
    if (stats && !strcmp(stats, "1"))
        fprintf(stderr, "PW_VK_REPLAY_POOL version=1 event=%s logical=%p requested=%u slots=%u limit=%u result=%d\\n",
                event, (const void *)pool, requested && !strcmp(requested, "1"), pool->slot_count, pool->slot_limit, result);
}

/* Extra pool aliases use the same logical client handle without modifying the
 * client object's unix_handle. Domain addresses remain stable until destroy. */
static unsigned pw_vk_pool_select(struct vulkan_device *device, struct wine_cmd_pool *pool, VkResult *status)
{
    unsigned slot = pool->next_slot++ % pool->slot_limit;
    if (slot >= pool->slot_count)
    {
        VkCommandPoolCreateInfo info = {0};
        VkCommandPool physical;
        VkResult result;
        if (pw_vk_async_pool_fanout_count() < 2)
            return slot % pool->slot_count;
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        info.flags = pool->create_flags;
        info.queueFamilyIndex = pool->queue_family;
        result = device->p_vkCreateCommandPool(device->host.device, &info, NULL, &physical);
        if (result != VK_SUCCESS)
        {
            /* Extra pools are an optional optimization; retain existing slots.
             * A future allocation will not repeatedly retry memory pressure. */
            if (result != VK_ERROR_OUT_OF_HOST_MEMORY && result != VK_ERROR_OUT_OF_DEVICE_MEMORY)
                *status = result;
            pool->slot_limit = pool->slot_count;
            pw_vk_pool_report(pool, "degrade", result);
            return slot % pool->slot_count;
        }
        slot = pool->slot_count++;
        pool->slots[slot].host_handle = physical;
        pool->slots[slot].client_handle = pool->obj.client_handle;
        device->physical_device->instance->p_insert_object(device->physical_device->instance, &pool->slots[slot]);
        pw_vk_pool_report(pool, "grow", VK_SUCCESS);
    }
    return slot;
}

VkResult wine_vkResetCommandPool(VkDevice handle, VkCommandPool command_pool, VkCommandPoolResetFlags flags)
{
    struct vulkan_device *device = vulkan_device_from_handle(handle);
    struct wine_cmd_pool *pool = wine_cmd_pool_from_handle(command_pool);
    VkResult result = VK_SUCCESS, current;
    unsigned i;
    pw_vk_async_wait_pool(command_pool);
    for (i = 0; i < pool->slot_count; ++i)
    {
        current = device->p_vkResetCommandPool(device->host.device, pool->slots[i].host_handle, flags);
        if (result == VK_SUCCESS) result = current;
    }
    return result;
}

void wine_vkTrimCommandPool(VkDevice handle, VkCommandPool command_pool, VkCommandPoolTrimFlags flags)
{
    struct vulkan_device *device = vulkan_device_from_handle(handle);
    struct wine_cmd_pool *pool = wine_cmd_pool_from_handle(command_pool);
    unsigned i;
    pw_vk_async_wait_pool(command_pool);
    for (i = 0; i < pool->slot_count; ++i)
        device->p_vkTrimCommandPool(device->host.device, pool->slots[i].host_handle, flags);
}

void wine_vkTrimCommandPoolKHR(VkDevice handle, VkCommandPool command_pool, VkCommandPoolTrimFlags flags)
{
    struct vulkan_device *device = vulkan_device_from_handle(handle);
    struct wine_cmd_pool *pool = wine_cmd_pool_from_handle(command_pool);
    unsigned i;
    pw_vk_async_wait_pool(command_pool);
    for (i = 0; i < pool->slot_count; ++i)
        device->p_vkTrimCommandPoolKHR(device->host.device, pool->slots[i].host_handle, flags);
}

'''


def transform(header, source, thunks, generator):
    assert 'PW_VK_POOL_MAX_SLOTS' not in header, 'pool fanout already staged'
    # The Unix admission scope reads only these original WOW64 prefixes.
    # Refuse a Wine update that changes their layout before enabling fanout.
    for name, info in (('vkCreateCommandPool', 'pCreateInfo'), ('vkAllocateCommandBuffers', 'pAllocateInfo')):
        pattern = (r'static NTSTATUS thunk32_' + name + r'\(void \*args\)\n\{\n'
                   r'    struct\n    \{\n        PTR32 device;\n        PTR32 ' + info + r';\n')
        assert re.search(pattern, thunks), name + ': WOW64 prefix changed'
    for name in ('VkCommandPoolCreateInfo32', 'VkCommandBufferAllocateInfo32'):
        pattern = r'typedef struct ' + name + r'\n\{\n    VkStructureType sType;\n    PTR32 pNext;\n'
        assert re.search(pattern, thunks), name + ': WOW64 chain prefix changed'
    source = once(source, '#include <time.h>\n', '#include <time.h>\n#include <stdio.h>\n#include <stdlib.h>\n')
    header = once(header, '    VULKAN_OBJECT_HEADER( VkCommandPool, command_pool );\n};',
                  '    VULKAN_OBJECT_HEADER( VkCommandPool, command_pool );\n'
                  '#define PW_VK_POOL_MAX_SLOTS 8\n'
                  '    struct vulkan_object slots[PW_VK_POOL_MAX_SLOTS];\n'
                  '    unsigned slot_count, slot_limit, next_slot;\n'
                  '    VkCommandPoolCreateFlags create_flags;\n'
                  '    uint32_t queue_family;\n};')
    header = once(header, '    struct wine_cmd_pool *pool;\n    struct pw_vk_replay_lane *replay_lane;',
                  '    struct wine_cmd_pool *pool;\n    unsigned pool_slot;\n    struct pw_vk_replay_lane *replay_lane;')
    header += '\nVkResult wine_vkResetCommandPool(VkDevice, VkCommandPool, VkCommandPoolResetFlags);\n'
    header += 'void wine_vkTrimCommandPool(VkDevice, VkCommandPool, VkCommandPoolTrimFlags);\n'
    header += 'void wine_vkTrimCommandPoolKHR(VkDevice, VkCommandPool, VkCommandPoolTrimFlags);\n'
    source = once(source, 'static void wine_vk_free_command_buffers(', HELPERS + 'static void wine_vk_free_command_buffers(')
    source = once(source, '        device->p_vkFreeCommandBuffers(device->host.device, pool->host.command_pool, 1,',
                  '        device->p_vkFreeCommandBuffers(device->host.device,\n'
                  '                pool->slots[((struct wine_cmd_buffer *)buffer)->pool_slot].host_handle, 1,')
    source = once(source, '        VkCommandBuffer host_command_buffer, client_command_buffer = buffers[i];',
                  '        VkCommandBuffer host_command_buffer, client_command_buffer = buffers[i];\n'
                  '        unsigned pool_slot = pw_vk_pool_select(device, pool, &res);\n'
                  '        if (res != VK_SUCCESS) break;')
    source = once(source, '        allocate_info_host.commandPool = pool->host.command_pool;',
                  '        allocate_info_host.commandPool = pool->slots[pool_slot].host_handle;')
    source = once(source, '        ((struct wine_cmd_buffer *)buffer)->pool = pool;',
                  '        ((struct wine_cmd_buffer *)buffer)->pool = pool;\n'
                  '        ((struct wine_cmd_buffer *)buffer)->pool_slot = pool_slot;')
    source = once(source, '    vulkan_object_init_ptr(&object->obj, host_command_pool, &client_command_pool->obj);',
                  '    object->slot_count = 1;\n'
                  '    object->slot_limit = (!info->pNext && !allocator) ? pw_vk_async_pool_fanout_count() : 1;\n'
                  '    if (!object->slot_limit || object->slot_limit > PW_VK_POOL_MAX_SLOTS) object->slot_limit = 1;\n'
                  '    object->create_flags = info->flags;\n'
                  '    object->queue_family = info->queueFamilyIndex;\n'
                  '    object->slots[0].host_handle = host_command_pool;\n'
                  '    vulkan_object_init_ptr(&object->obj, host_command_pool, &client_command_pool->obj);')
    source = once(source, '    instance->p_insert_object(instance, &object->obj);\n\n    return VK_SUCCESS;\n}\n\nvoid wine_vkDestroyCommandPool',
                  '    instance->p_insert_object(instance, &object->obj);\n'
                  '    pw_vk_pool_report(object, "create", VK_SUCCESS);\n\n    return VK_SUCCESS;\n}\n\nvoid wine_vkDestroyCommandPool')
    source = once(source, '    device->p_vkDestroyCommandPool(device->host.device, pool->host.command_pool, NULL);',
                  '    for (unsigned i = 0; i < pool->slot_count; ++i)\n'
                  '    {\n'
                  '        device->p_vkDestroyCommandPool(device->host.device, pool->slots[i].host_handle, NULL);\n'
                  '        if (i) instance->p_remove_object(instance, &pool->slots[i]);\n'
                  '    }')
    for name in ('vkResetCommandPool', 'vkTrimCommandPool', 'vkTrimCommandPoolKHR'):
        for bits in (32, 64):
            pattern = r'(static (?:NTSTATUS|void) thunk' + str(bits) + '_' + name + r'\(void \*args\)\n\{.*?\n\})'
            matches = list(re.finditer(pattern, thunks, re.S))
            assert len(matches) == 1, (name, bits)
            match = matches[0]
            body = match[0]
            lines = body.splitlines(keepends=True)
            target = [line for line in lines if '->p_' + name + '(' in line]
            assert len(target) == 1, (name, bits, target)
            body = once(body, target[0], fanout_call(name, 'params->', bits == 32))
            thunks = thunks[:match.start()] + body + thunks[match.end():]
    generator = once(generator, 'class Function(', inspect.getsource(fanout_call) + '\n\nclass Function(')
    generator = once(generator, '        # Call the host Vulkan function.\n        if self.type == "void":',
                     '        # Call the host Vulkan function.\n'
                     '        fanout = fanout_call(self.name, params_prefix, conv)\n'
                     '        if fanout is not None:\n            body += fanout\n'
                     '        elif self.type == "void":')
    ast.parse(generator)
    return header, source, thunks, generator


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True)
    args = parser.parse_args()
    directory = Path(args.source) / 'dlls/winevulkan'
    paths = [directory / name for name in ('vulkan_private.h', 'vulkan.c', 'vulkan_thunks.c', 'make_vulkan')]
    results = transform(*(path.read_text() for path in paths))
    for path, result in zip(paths, results):
        path.write_text(result)


if __name__ == '__main__':
    main()
