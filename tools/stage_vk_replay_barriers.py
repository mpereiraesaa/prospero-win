#!/usr/bin/env python3
"""Stage command-pool lifetime and targeted submit barriers into pinned Wine."""
import argparse
import ast
import inspect
import re
from pathlib import Path


def once(text, old, new):
    assert text.count(old) == 1, (old, text.count(old))
    return text.replace(old, new)


def replay_barrier(name, conv, prefix="params->"):
    """Emit barriers before conversion destroys original client handles."""
    def handle(field):
        value = prefix + field
        return "(VkCommandBuffer)UlongToPtr(" + value + ")" if conv else value

    if name in ("vkBeginCommandBuffer", "vkEndCommandBuffer", "vkResetCommandBuffer"):
        return "    pw_vk_async_wait_buffer_pool(" + handle("commandBuffer") + ");\n"
    if name in ("vkResetCommandPool", "vkTrimCommandPool", "vkTrimCommandPoolKHR"):
        return "    pw_vk_async_wait_pool(" + prefix + "commandPool);\n"
    if name == "vkCmdExecuteCommands":
        ptr = "(const PTR32 *)UlongToPtr(" + prefix + "pCommandBuffers)" if conv else prefix + "pCommandBuffers"
        buffer = "(VkCommandBuffer)UlongToPtr(pw_buffers[pw_i])" if conv else "pw_buffers[pw_i]"
        return ("    {\n        uint32_t pw_i;\n        const " + ("PTR32" if conv else "VkCommandBuffer") +
                " *pw_buffers = " + ptr + ";\n        pw_vk_async_wait_buffer_pool(" + handle("commandBuffer") +
                ");\n        for (pw_i = 0; pw_i < " + prefix + "commandBufferCount; ++pw_i)\n"
                "            pw_vk_async_wait_buffer(" + buffer + ");\n    }\n")
    if name not in ("vkQueueSubmit", "vkQueueSubmit2", "vkQueueSubmit2KHR"):
        return ""
    is_two = name != "vkQueueSubmit"
    typ = "VkSubmitInfo2" if is_two else "VkSubmitInfo"
    if conv:
        typ += "32"
    ptr = "(const " + typ + " *)UlongToPtr(" + prefix + "pSubmits)" if conv else prefix + "pSubmits"
    element = "VkCommandBufferSubmitInfo" + ("32" if conv else "") if is_two else ("PTR32" if conv else "VkCommandBuffer")
    field = "pCommandBufferInfos" if is_two else "pCommandBuffers"
    count = "commandBufferInfoCount" if is_two else "commandBufferCount"
    array = "pw_submits[pw_i]." + field
    if conv:
        array = "(const " + element + " *)UlongToPtr(" + array + ")"
    buffer = "pw_buffers[pw_j]" + (".commandBuffer" if is_two else "")
    if conv:
        buffer = "(VkCommandBuffer)UlongToPtr(" + buffer + ")"
    return ("    {\n        uint32_t pw_i, pw_j;\n        const " + typ + " *pw_submits = " + ptr +
            ";\n        for (pw_i = 0; pw_i < " + prefix + "submitCount; ++pw_i)\n        {\n"
            "            const " + element + " *pw_buffers = " + array + ";\n"
            "            for (pw_j = 0; pw_j < pw_submits[pw_i]." + count + "; ++pw_j)\n"
            "                pw_vk_async_wait_buffer(" + buffer + ");\n        }\n    }\n")


TARGETS = ("vkBeginCommandBuffer", "vkEndCommandBuffer", "vkResetCommandBuffer",
           "vkResetCommandPool", "vkTrimCommandPool", "vkTrimCommandPoolKHR",
           "vkCmdExecuteCommands", "vkQueueSubmit", "vkQueueSubmit2", "vkQueueSubmit2KHR")


def transform(header, source, thunks, generator):
    assert '#include "pw_vk_async.h"' not in header, 'replay barriers already staged'
    header = once(header, 'struct pw_vk_replay_lane;', '#include "pw_vk_async.h"\n\nstruct pw_vk_replay_lane;')
    # Allocation/free/pool destruction are manually implemented, so generated
    # thunks must not duplicate these hooks. Partial allocation cleanup uses the
    # same free helper and therefore retires each lane before freeing its owner.
    source = once(source, '    pool = wine_cmd_pool_from_handle(allocate_info->commandPool);',
                  '    pool = wine_cmd_pool_from_handle(allocate_info->commandPool);\n'
                  '    pw_vk_async_wait_pool(allocate_info->commandPool);')
    source = once(source, '    wine_vk_free_command_buffers(device, pool, count, buffers);',
                  '    pw_vk_async_wait_pool(command_pool);\n'
                  '    wine_vk_free_command_buffers(device, pool, count, buffers);')
    source = once(source, '        device->p_vkFreeCommandBuffers(device->host.device, pool->host.command_pool, 1,',
                  '        pw_vk_async_forget_buffer(buffers[i]);\n'
                  '        device->p_vkFreeCommandBuffers(device->host.device, pool->host.command_pool, 1,')
    source = once(source, '    device->p_vkDestroyCommandPool(device->host.device, pool->host.command_pool, NULL);',
                  '    pw_vk_async_wait_pool(handle);\n'
                  '    device->p_vkDestroyCommandPool(device->host.device, pool->host.command_pool, NULL);')
    for name in TARGETS:
        for bits in (32, 64):
            pattern = r'(static (?:NTSTATUS|void) thunk' + str(bits) + '_' + name + r'\(void \*args\)\n\{.*?\n\n)'
            matches = list(re.finditer(pattern, thunks, re.S))
            assert len(matches) == 1, (name, bits, len(matches))
            match = matches[0]
            thunks = thunks[:match.end()] + replay_barrier(name, bits == 32) + thunks[match.end():]
    # Embed exactly the same emitter in Wine's generator. Regeneration must be
    # byte identical to the directly staged thunk file.
    emitter = inspect.getsource(replay_barrier) + '\n\n'
    generator = once(generator, 'class Function(', emitter + 'class Function(')
    generator = once(generator, '        if not self.is_perf_critical():\n            body += "    {0}\\n".format(self.trace(params_prefix=params_prefix, conv=conv))',
                     '        body += replay_barrier(self.name, conv, params_prefix)\n\n'
                     '        if not self.is_perf_critical():\n            body += "    {0}\\n".format(self.trace(params_prefix=params_prefix, conv=conv))')
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
