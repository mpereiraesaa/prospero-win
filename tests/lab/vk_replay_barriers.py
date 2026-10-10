#!/usr/bin/env python3
"""Check pinned staging, exact Wine regeneration, and compiled barrier traversal."""
import argparse
import hashlib
import io
import json
import logging
import os
import re
import resource
import runpy
import subprocess
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', required=True)
p.add_argument('--vk-xml', required=True)
p.add_argument('--video-xml', required=True)
p.add_argument('--output', required=True)
a = p.parse_args()
repo = Path(__file__).resolve().parents[2]
out = Path(a.output).resolve()
out.mkdir(parents=True, exist_ok=True)
d = Path(a.source).resolve() / 'dlls/winevulkan'
life = runpy.run_path(str(repo / 'tools/stage_vk_replay_lifecycle.py'))
stage = runpy.run_path(str(repo / 'tools/stage_vk_replay_barriers.py'))
h, c = life['transform']((d / 'vulkan_private.h').read_text(), (d / 'vulkan.c').read_text())
inputs = (h, c, (d / 'vulkan_thunks.c').read_text(), (d / 'make_vulkan').read_text())
header, code, thunks, generator = stage['transform'](*inputs)
assert (header, code, thunks, generator) == stage['transform'](*inputs)
try:
    stage['transform'](header, code, thunks, generator)
except AssertionError:
    pass
else:
    raise AssertionError('duplicate staging accepted')
for name, before, after in (
    ('wine_vkAllocateCommandBuffers', 'pw_vk_async_wait_pool(allocate_info->commandPool)', 'device->p_vkAllocateCommandBuffers'),
    ('wine_vkFreeCommandBuffers', 'pw_vk_async_wait_pool(command_pool)', 'wine_vk_free_command_buffers'),
    ('wine_vkDestroyCommandPool', 'pw_vk_async_wait_pool(handle)', 'device->p_vkDestroyCommandPool'),
    ('wine_vk_free_command_buffers', 'pw_vk_async_forget_buffer(buffers[i])', 'device->p_vkFreeCommandBuffers'),
):
    start = code.index(name + '(')
    body = code[start:code.index('\n}\n', start)]
    # Skip the function signature when the body calls a similarly named helper.
    body = body[body.index('{'):]
    assert body.index(before) < body.index(after), name

# Run the real patched generator without writing any original source files.
(out / 'make_vulkan').write_text(generator)
module = runpy.run_path(str(out / 'make_vulkan'))
module['LOGGER'].setLevel(logging.WARNING)
vk_xml, video_xml = str(Path(a.vk_xml).resolve()), str(Path(a.video_xml).resolve())
os.chdir(d)
g = module['Generator'](vk_xml, video_xml)
regenerated = io.StringIO()
g.generate_thunks_c(regenerated)
assert regenerated.getvalue() == thunks, 'Wine regeneration differs from direct staging'
(out / 'vulkan_thunks.c').write_text(thunks)

fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef void *VkCommandBuffer;
typedef uint64_t VkCommandPool;
typedef uint32_t PTR32;
static void *pointers[256];
#define UlongToPtr(v) pointers[(v)]
typedef struct {uint32_t commandBufferCount;const VkCommandBuffer *pCommandBuffers;} VkSubmitInfo;
typedef struct {uint32_t commandBufferCount;PTR32 pCommandBuffers;} VkSubmitInfo32;
typedef struct {VkCommandBuffer commandBuffer;} VkCommandBufferSubmitInfo;
typedef struct {PTR32 commandBuffer;} VkCommandBufferSubmitInfo32;
typedef struct {uint32_t commandBufferInfoCount;const VkCommandBufferSubmitInfo *pCommandBufferInfos;} VkSubmitInfo2;
typedef struct {uint32_t commandBufferInfoCount;PTR32 pCommandBufferInfos;} VkSubmitInfo232;
static uintptr_t seen[16];static unsigned calls,kinds[16];
static void pw_vk_async_wait_buffer(VkCommandBuffer b){seen[calls]=(uintptr_t)b;kinds[calls++]=1;}
static void pw_vk_async_wait_buffer_pool(VkCommandBuffer b){seen[calls]=(uintptr_t)b;kinds[calls++]=2;}
static void pw_vk_async_wait_pool(VkCommandPool p){seen[calls]=(uintptr_t)p;kinds[calls++]=3;}
'''
# Extract the actual injected prefix from each staged thunk, not a second copy
# of the emitter. Compiled pointer traversal exercises both native and WOW64.
for name in stage['TARGETS']:
    for bits in (32, 64):
        prefix = re.search(r'static (?:void|NTSTATUS) thunk' + str(bits) + '_' + name + r'\(void \*args\)\n\{.*?\n\n', thunks, re.S)
        expected = stage['replay_barrier'](name, bits == 32)
        fragment = thunks[prefix.end():prefix.end() + len(expected)]
        assert fragment == expected
        if name.startswith('vkQueueSubmit'):
            typ = 'VkSubmitInfo2' if name != 'vkQueueSubmit' else 'VkSubmitInfo'
            fields = 'uint32_t submitCount; ' + ('PTR32' if bits == 32 else 'const ' + typ + ' *') + ' pSubmits;'
        elif name == 'vkCmdExecuteCommands':
            fields = ('PTR32' if bits == 32 else 'VkCommandBuffer') + ' commandBuffer; uint32_t commandBufferCount; ' + ('PTR32' if bits == 32 else 'const VkCommandBuffer *') + ' pCommandBuffers;'
        elif 'Pool' in name:
            fields = 'VkCommandPool commandPool;'
        else:
            fields = ('PTR32' if bits == 32 else 'VkCommandBuffer') + ' commandBuffer;'
        fixture += '\nstruct p_' + name + str(bits) + ' {' + fields + '};\n'
        fixture += 'static void call_' + name + str(bits) + '(struct p_' + name + str(bits) + ' *params)\n{\n' + fragment + '}\n'
fixture += r'''
int main(void){
 VkCommandBuffer buffers[3]={(void *)0x111,(void *)0x222,(void *)0x333};
 PTR32 buffers32[3]={1,2,3};
 VkSubmitInfo submits[2]={{2,buffers},{1,buffers+2}};
 VkSubmitInfo32 submits32[2]={{2,10},{1,11}};
 VkCommandBufferSubmitInfo infos[3]={{(void *)0x111},{(void *)0x222},{(void *)0x333}};
 VkCommandBufferSubmitInfo32 infos32[3]={{1},{2},{3}};
 VkSubmitInfo2 submits2[2]={{2,infos},{1,infos+2}};
 VkSubmitInfo232 submits232[2]={{2,12},{1,13}};
 pointers[1]=buffers[0];pointers[2]=buffers[1];pointers[3]=buffers[2];
 pointers[10]=buffers32;pointers[11]=buffers32+2;pointers[12]=infos32;pointers[13]=infos32+2;
 pointers[20]=submits32;pointers[21]=submits232;
'''
for name in stage['TARGETS']:
    for bits in (32, 64):
        struct = 'struct p_' + name + str(bits)
        call = 'call_' + name + str(bits)
        fixture += ' calls=0;\n'
        if name.startswith('vkQueueSubmit'):
            array = ('20' if name == 'vkQueueSubmit' else '21') if bits == 32 else ('submits' if name == 'vkQueueSubmit' else 'submits2')
            fixture += ' ' + call + '(&(' + struct + '){2,' + array + '});\n'
            fixture += ' assert(calls==3);for(unsigned i=0;i<3;i++){assert(kinds[i]==1);assert(seen[i]==(uintptr_t)buffers[i]);}\n'
            fixture += ' calls=0;' + call + '(&(' + struct + '){0,0});assert(!calls);\n'
        elif name == 'vkCmdExecuteCommands':
            values = '1,3,10' if bits == 32 else 'buffers[0],3,buffers'
            fixture += ' ' + call + '(&(' + struct + '){' + values + '});\n'
            fixture += ' assert(calls==4 && kinds[0]==2 && seen[0]==(uintptr_t)buffers[0]);\n'
            fixture += ' for(unsigned i=0;i<3;i++){assert(kinds[i+1]==1);assert(seen[i+1]==(uintptr_t)buffers[i]);}\n'
        elif 'Pool' in name:
            fixture += ' ' + call + '(&(' + struct + '){0x777});assert(calls==1 && kinds[0]==3 && seen[0]==0x777);\n'
        else:
            fixture += ' ' + call + '(&(' + struct + '){' + ('1' if bits == 32 else 'buffers[0]') + '});assert(calls==1 && kinds[0]==2 && seen[0]==(uintptr_t)buffers[0]);\n'
fixture += ' puts("PASS staged lifecycle and targeted submit barriers, both pointer widths");return 0;}\n'
(out / 'fixture.c').write_text(fixture)
commands = []
for suffix, flags in (('host', []), ('san', ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie'])):
    command = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', *flags, str(out / 'fixture.c'), '-o', str(out / suffix)]
    subprocess.run(command, check=True)
    result = subprocess.run([str(out / suffix)], text=True, capture_output=True, check=True)
    commands.append({'command': command, 'stdout': result.stdout})
# Prove the fixture detects an omitted synchronization hook.
negative = fixture.replace('pw_vk_async_wait_buffer_pool((VkCommandBuffer)UlongToPtr(params->commandBuffer));', '', 1)
assert negative != fixture
(out / 'negative.c').write_text(negative)
subprocess.run(['cc', '-std=c11', str(out / 'negative.c'), '-o', str(out / 'negative')], check=True)
result = subprocess.run([str(out / 'negative')], capture_output=True, preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
assert result.returncode == -6 and b'Assertion' in result.stderr
receipt = {'status':'PASS', 'thunks':20, 'generator_sha256':hashlib.sha256(generator.encode()).hexdigest(),
           'regenerated_thunks_sha256':hashlib.sha256(thunks.encode()).hexdigest(), 'runs':commands, 'negative_exit':result.returncode}
(out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(json.dumps(receipt, indent=2))
