#!/usr/bin/env python3
"""Fail-closed native replay classification from the exact staged Wine thunks."""
import argparse
import json
import pathlib
import re


def function_body(source, signature):
    match = re.search(signature + r'\s*\{', source)
    if not match:
        return None
    begin = match.end()
    depth = 1
    for end in range(begin, len(source)):
        depth += (source[end] == '{') - (source[end] == '}')
        if not depth:
            return source[begin:end]
    raise ValueError('unterminated function')


def classify(thunks, header, driver):
    # This is the entire transitive Wine helper chain accepted below. Changes
    # require review; accepting arbitrary calls here would admit Wine TLS users.
    unwrap = function_body(driver, r'static inline struct vulkan_command_buffer \*vulkan_command_buffer_from_handle\(\s*VkCommandBuffer handle\s*\)')
    expected = ('struct vulkan_client_object *client = (struct vulkan_client_object *)handle; '
                'return (struct vulkan_command_buffer *)(UINT_PTR)client->unix_handle;')
    if unwrap is None or re.sub(r'\s+', '', unwrap) != re.sub(r'\s+', '', expected):
        raise ValueError('command-buffer unwrap changed; worker safety needs review')
    eligible = []
    excluded = {}
    names = sorted(set(re.findall(r'static void thunk64_(vkCmd\w+)\(void \*args\)', thunks)))
    for name in names:
        if any(word in name for word in ('ExecuteCommands', 'Debug', 'Checkpoint')):
            excluded[name] = 'secondary dependency or diagnostic callback'
            continue
        body = function_body(thunks, rf'static void thunk64_{name}\(void \*args\)')
        # Every argument must be a direct member of the decoded native params.
        # No wrappers, conversions, allocation, debug macros or TLS can pass.
        grammar = (rf'\s*struct {name}_params \*params = args;\s*'
                   rf'vulkan_command_buffer_from_handle\(params->commandBuffer\)->device->p_{name}\('
                   r'vulkan_command_buffer_from_handle\(params->commandBuffer\)->host.command_buffer'
                   r'(?:, params->\w+)*\);\s*')
        if not re.fullmatch(grammar, body or ''):
            excluded[name] = 'native thunk is not a direct driver call'
            continue
        struct = re.search(rf'struct {name}_params\s*\{{([^}}]+)\}};', header)
        if not struct or not re.search(r'\bVkCommandBuffer commandBuffer;', struct[1]):
            raise ValueError(name + ': missing native commandBuffer member')
        if not re.search(rf'\bunix_{name},', header):
            raise ValueError(name + ': missing Unix enum')
        eligible.append(name)
    if not eligible:
        raise ValueError('no eligible native command thunks')
    return eligible, excluded


def generate(eligible):
    return ('/* Generated from inspected native Wine thunks. Do not edit. */\n'
            '#ifndef PW_VK_REPLAY_DISPATCH_H\n#define PW_VK_REPLAY_DISPATCH_H\n'
            'static VkCommandBuffer pw_vk_replay_command_buffer(unsigned code, const void *decoded_native_params)\n'
            '{\n#ifdef _WIN64\n    if (!decoded_native_params) return (VkCommandBuffer)0;\n'
            '    switch (code)\n    {\n' + ''.join(
                f'    case unix_{name}: return ((const struct {name}_params *)decoded_native_params)->commandBuffer;\n'
                for name in eligible) +
            '    default: break;\n    }\n#else\n    (void)code; (void)decoded_native_params;\n#endif\n'
            '    return (VkCommandBuffer)0;\n}\n#endif\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--driver-header', help='Exact matching Wine header when source staging is partial')
    args = parser.parse_args()
    source = pathlib.Path(args.source)
    directory = source / 'dlls/winevulkan'
    eligible, excluded = classify((directory / 'vulkan_thunks.c').read_text(),
                                  (directory / 'loader_thunks.h').read_text(),
                                  (pathlib.Path(args.driver_header) if args.driver_header else source / 'include/wine/vulkan_driver.h').read_text())
    output = pathlib.Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    (output / 'pw_vk_replay_dispatch.h').write_text(generate(eligible))
    (output / 'pw_vk_replay_dispatch.json').write_text(json.dumps(
        {'eligible': eligible, 'excluded': excluded, 'native_bits': 64}, indent=2) + '\n')


if __name__ == '__main__':
    main()
