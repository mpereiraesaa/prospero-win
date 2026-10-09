#!/usr/bin/env python3
"""Classifier tests, optionally compiling against the actual pinned native ABI."""
import argparse
import importlib.util
import pathlib
import subprocess
import tempfile
import unittest
import sys
sys.dont_write_bytecode = True

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('dispatch', ROOT / 'tools/generate_vk_replay_dispatch.py')
dispatch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dispatch)
DRIVER = '''static inline struct vulkan_command_buffer *vulkan_command_buffer_from_handle( VkCommandBuffer handle )
{
    struct vulkan_client_object *client = (struct vulkan_client_object *)handle;
    return (struct vulkan_command_buffer *)(UINT_PTR)client->unix_handle;
}
'''


def thunk(name, extra=''):
    return f'''static void thunk64_{name}(void *args)
{{
    struct {name}_params *params = args;
    {extra}
    vulkan_command_buffer_from_handle(params->commandBuffer)->device->p_{name}(vulkan_command_buffer_from_handle(params->commandBuffer)->host.command_buffer, params->value);
}}
'''


def header(names):
    return '\n'.join(f'unix_{name},\nstruct {name}_params {{ VkCommandBuffer commandBuffer; unsigned value; }};' for name in names)


class ClassifierTests(unittest.TestCase):
    def test_direct_only(self):
        names = ['vkCmdDraw', 'vkCmdExecuteCommands', 'vkCmdInsertDebugUtilsLabelEXT', 'vkCmdFoo']
        text = ''.join(thunk(n, 'TRACE("unsafe");' if n == 'vkCmdFoo' else '') for n in names)
        eligible, excluded = dispatch.classify(text, header(names), DRIVER)
        self.assertEqual(eligible, ['vkCmdDraw'])
        self.assertEqual(len(excluded), 3)
        self.assertEqual(dispatch.generate(eligible), dispatch.generate(eligible))

    def test_helper_changes_fail_closed(self):
        with self.assertRaises(ValueError):
            dispatch.classify(thunk('vkCmdDraw'), header(['vkCmdDraw']), DRIVER.replace('return ', 'TRACE("TLS"); return '))

    def test_wrong_member_fails(self):
        with self.assertRaises(ValueError):
            dispatch.classify(thunk('vkCmdDraw'), header(['vkCmdDraw']).replace('VkCommandBuffer', 'unsigned'), DRIVER)

    def test_conversion_is_not_direct(self):
        source = thunk('vkCmdDraw') + thunk('vkCmdFoo').replace('params->value);', 'convert(params->value));')
        eligible, excluded = dispatch.classify(source, header(['vkCmdDraw', 'vkCmdFoo']), DRIVER)
        self.assertEqual(eligible, ['vkCmdDraw'])
        self.assertIn('vkCmdFoo', excluded)

    def test_ordered_updates_fail_closed(self):
        device_driver = DRIVER.replace('vulkan_command_buffer', 'vulkan_device').replace('VkCommandBuffer', 'VkDevice')
        name = 'vkUpdateDescriptorSets'
        source = thunk(name).replace('vulkan_command_buffer_from_handle(params->commandBuffer)->device', 'vulkan_device_from_handle(params->device)').replace('vulkan_command_buffer_from_handle(params->commandBuffer)->host.command_buffer', 'vulkan_device_from_handle(params->device)->host.device')
        native_header = header([name]).replace('VkCommandBuffer commandBuffer', 'VkDevice device')
        self.assertEqual(dispatch.classify_ordered(source, native_header, device_driver), [name])
        self.assertEqual(dispatch.classify_ordered(source.replace('struct '+name+'_params *params = args;', 'struct '+name+'_params *params = args; TRACE("unsafe");'), native_header, device_driver), [])
        self.assertEqual(dispatch.classify_ordered(source.replace(name, 'vkDestroyDescriptorPool'), native_header.replace(name, 'vkDestroyDescriptorPool'), device_driver), [])
        with self.assertRaises(ValueError):
            dispatch.classify_ordered(source, native_header, device_driver.replace('return ', 'TRACE("TLS"); return '))
        with self.assertRaises(ValueError):
            dispatch.classify_ordered(source, native_header.replace('VkDevice device', 'unsigned device'), device_driver)


def actual_abi(source):
    source = pathlib.Path(source)
    directory = source / 'dlls/winevulkan'
    eligible, excluded = dispatch.classify((directory / 'vulkan_thunks.c').read_text(),
                                           (directory / 'loader_thunks.h').read_text(),
                                           (source / 'include/wine/vulkan_driver.h').read_text())
    ordered = dispatch.classify_ordered((directory / 'vulkan_thunks.c').read_text(), (directory / 'loader_thunks.h').read_text(), (source / 'include/wine/vulkan_driver.h').read_text())
    assert ordered == ['vkUpdateDescriptorSets', 'vkUpdateDescriptorSetWithTemplate'], ordered
    required = ['vkCmdDraw', 'vkCmdDrawIndexed', 'vkCmdBindPipeline', 'vkCmdPipelineBarrier',
                'vkCmdBindDescriptorSets', 'vkCmdCopyBuffer', 'vkCmdDispatch']
    assert all(name in eligible for name in required), eligible
    assert 'vkCmdExecuteCommands' not in eligible
    with tempfile.TemporaryDirectory(prefix='vk-replay-dispatch-') as temporary:
        work = pathlib.Path(temporary)
        (work / 'pw_vk_replay_dispatch.h').write_text(dispatch.generate(eligible, ordered))
        (work / 'fixture.c').write_text('''#define WINE_UNIX_LIB
#include <assert.h>
#include <stdint.h>
#include "windef.h"
#include "winbase.h"
#include "wine/vulkan.h"
#include "loader_thunks.h"
#include "pw_vk_replay_dispatch.h"
int main(void) {
''' + ''.join(f'''    {{ struct {name}_params p = {{0}};
    p.commandBuffer = (VkCommandBuffer)(uintptr_t)0x123456789abcdef0ULL;
#ifdef _WIN64
    assert(pw_vk_replay_command_buffer(unix_{name}, &p) == p.commandBuffer);
#else
    assert(!pw_vk_replay_command_buffer(unix_{name}, &p));
#endif
    }}
''' for name in eligible) + ''.join(f'''#ifdef _WIN64
    assert(pw_vk_replay_ordered_update(unix_{name}));
#else
    assert(!pw_vk_replay_ordered_update(unix_{name}));
#endif
''' for name in ordered) + '''    assert(!pw_vk_replay_ordered_update(unix_vkDestroyDescriptorPool));
    assert(!pw_vk_replay_command_buffer(unix_vkCmdExecuteCommands, 0));
    assert(!pw_vk_replay_command_buffer(unix_vkQueueSubmit, 0));
    assert(!pw_vk_replay_command_buffer(~0u, 0));
    assert(!pw_vk_replay_command_buffer(unix_vkCmdDraw, 0));
    return 0;
}
''')
        for native in (True, False):
            command = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(source / 'include'),
                       '-I' + str(directory), '-I' + str(work)]
            if native:
                command += ['-D_WIN64']
            subprocess.run(command + [str(work / 'fixture.c'), '-o', str(work / 'fixture')], check=True)
            subprocess.run([str(work / 'fixture')], check=True)
    print(f'Actual native ABI: {len(eligible)} eligible command types; extraction and disabled path passed')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--source')
    args = parser.parse_args()
    result = unittest.TextTestRunner().run(unittest.defaultTestLoader.loadTestsFromTestCase(ClassifierTests))
    if not result.wasSuccessful():
        raise SystemExit(1)
    if args.source:
        actual_abi(args.source)
