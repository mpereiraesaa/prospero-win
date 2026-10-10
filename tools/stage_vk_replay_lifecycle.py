#!/usr/bin/env python3
"""Attach private pool membership to Wine's native command-buffer wrappers.

This is metadata only; worker dispatch and lifetime barriers are separate.
Use an isolated source tree. Refuse unexpected or already-modified anchors.
"""
import argparse
from pathlib import Path


def once(text, old, new):
    assert text.count(old) == 1, (old, text.count(old))
    return text.replace(old, new)


def transform(header, source):
    private = '''/* Native-only tail: keep the shared Vulkan object prefix unchanged. */
struct pw_vk_replay_lane;
struct wine_cmd_buffer
{
    struct vulkan_command_buffer obj;
    struct wine_cmd_pool *pool;
    struct pw_vk_replay_lane *replay_lane;
};
C_ASSERT(offsetof(struct wine_cmd_buffer, obj) == 0);

static inline struct wine_cmd_buffer *wine_cmd_buffer_from_handle(VkCommandBuffer handle)
{
    return (struct wine_cmd_buffer *)vulkan_command_buffer_from_handle(handle);
}

'''
    header = once(header, 'BOOL wine_vk_is_type_wrapped(VkObjectType type);',
                  private + 'BOOL wine_vk_is_type_wrapped(VkObjectType type);')
    start = source.index('VkResult wine_vkAllocateCommandBuffers(')
    end = source.index('\n}\n', start) + 3
    body = source[start:end]
    body = once(body, 'buffer = calloc(1, sizeof(*buffer))',
                'buffer = calloc(1, sizeof(struct wine_cmd_buffer))')
    body = once(body, '        buffer->device = device;',
                '        buffer->device = device;\n'
                '        ((struct wine_cmd_buffer *)buffer)->pool = pool;')
    return header, source[:start] + body + source[end:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True)
    args = parser.parse_args()
    directory = Path(args.source) / 'dlls/winevulkan'
    hp, cp = directory / 'vulkan_private.h', directory / 'vulkan.c'
    header, source = transform(hp.read_text(), cp.read_text())
    hp.write_text(header)
    cp.write_text(source)


if __name__ == '__main__':
    main()
