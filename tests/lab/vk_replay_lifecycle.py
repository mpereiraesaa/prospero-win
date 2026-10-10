#!/usr/bin/env python3
"""Compile staged Wine lifecycle bodies with controlled native driver functions.

No Vulkan driver, guest execution or console access. Only output copies change.
"""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import resource
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', required=True)
p.add_argument('--build', required=True)
p.add_argument('--output', required=True)
a = p.parse_args()
repo = Path(__file__).resolve().parents[2]
source, build, out = Path(a.source).resolve(), Path(a.build).resolve(), Path(a.output).resolve()
out.mkdir(parents=True, exist_ok=True)
stage = runpy.run_path(str(repo / 'tools/stage_vk_replay_lifecycle.py'))
d = source / 'dlls/winevulkan'
original_header, original_source = (d / 'vulkan_private.h').read_text(), (d / 'vulkan.c').read_text()
header, code = stage['transform'](original_header, original_source)
assert (header, code) == stage['transform'](original_header, original_source)
try:
    stage['transform'](header, code)
except AssertionError:
    pass
else:
    raise AssertionError('duplicate staging accepted')
(out / 'vulkan_private.h').write_text(header)

def body(name):
    start = code.index(name + '(')
    start = code.rfind('\n', 0, start) + 1
    end = code.index('\n}\n', start) + 3
    return code[start:end]

fixture = r'''
#include "vulkan_private.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef TRACE
#undef ERR
#undef FIXME
#define TRACE(...) ((void)0)
#define ERR(...) ((void)0)
#define FIXME(...) ((void)0)
static unsigned allocations,fail_alloc,live,inserted,removed,driver_calls,fail_driver,freed;
static void *tracked_calloc(size_t n,size_t size)
{
 if(++allocations==fail_alloc)return NULL;
 void *p=calloc(n,size);if(p)++live;return p;
}
static void tracked_free(void *p){if(p){assert(live);--live;}free(p);}
#define calloc tracked_calloc
#define free tracked_free
'''
for name in ['wine_vk_free_command_buffers', 'wine_vkAllocateCommandBuffers',
             'wine_vkFreeCommandBuffers', 'wine_vkCreateCommandPool', 'wine_vkDestroyCommandPool']:
    fixture += body(name)
fixture += r'''
#undef calloc
#undef free
static void insert(struct vulkan_instance *i,struct vulkan_object *o)
{(void)i;assert(o->client_handle && o->host_handle);++inserted;}
static void remove_object(struct vulkan_instance *i,struct vulkan_object *o)
{(void)i;assert(o->client_handle && o->host_handle);++removed;}
static VkResult create_pool(VkDevice d,const VkCommandPoolCreateInfo *info,const VkAllocationCallbacks *a,VkCommandPool *p)
{(void)d;assert(!a);*p=0x100+info->queueFamilyIndex;return VK_SUCCESS;}
static void destroy_pool(VkDevice d,VkCommandPool p,const VkAllocationCallbacks *a)
{(void)d;assert(!a && p>=0x100);}
static VkResult allocate(VkDevice d,const VkCommandBufferAllocateInfo *info,VkCommandBuffer *buffer)
{
 (void)d;assert(info->commandBufferCount==1);assert(info->commandPool>=0x100);
 if(++driver_calls==fail_driver)return VK_ERROR_OUT_OF_DEVICE_MEMORY;
 *buffer=(VkCommandBuffer)(uintptr_t)(info->commandPool*100+driver_calls);return VK_SUCCESS;
}
static void release_buffers(VkDevice d,VkCommandPool p,uint32_t count,const VkCommandBuffer *buffers)
{(void)d;assert(count==1);assert((uintptr_t)*buffers/100==p);++freed;}
int main(void)
{
 struct vulkan_instance instance={0};struct vulkan_physical_device physical={0};struct vulkan_device device={0};
 struct VkDevice_T client_device={0};struct vk_command_pool client_pools[2]={{0}};
 struct VkCommandBuffer_T client_buffers[3]={{0}};VkCommandBuffer buffers[3];VkCommandPool pools[2];
 VkCommandPoolCreateInfo pool_info={0};VkCommandBufferAllocateInfo info={0};
 instance.p_insert_object=insert;instance.p_remove_object=remove_object;
 physical.instance=&instance;device.physical_device=&physical;
 device.p_vkCreateCommandPool=create_pool;device.p_vkDestroyCommandPool=destroy_pool;
 device.p_vkAllocateCommandBuffers=allocate;device.p_vkFreeCommandBuffers=release_buffers;
 client_device.obj.unix_handle=(uintptr_t)&device;
 pool_info.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
 for(unsigned i=0;i<2;i++){
  pools[i]=(uintptr_t)&client_pools[i];pool_info.queueFamilyIndex=i;
  assert(wine_vkCreateCommandPool(&client_device,&pool_info,NULL,&pools[i])==VK_SUCCESS);
 }
 info.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;info.commandBufferCount=3;
 for(unsigned round=0;round<2;round++){
  info.commandPool=pools[round];info.level=round?VK_COMMAND_BUFFER_LEVEL_SECONDARY:VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  for(unsigned i=0;i<3;i++)buffers[i]=&client_buffers[i];
  assert(wine_vkAllocateCommandBuffers(&client_device,&info,buffers)==VK_SUCCESS);
  for(unsigned i=0;i<3;i++){
   struct wine_cmd_buffer *b=wine_cmd_buffer_from_handle(buffers[i]);
   assert(b->pool==wine_cmd_pool_from_handle(pools[round]));assert(b->obj.device==&device);
   assert(!b->replay_lane);assert(&b->obj==vulkan_command_buffer_from_handle(buffers[i]));
  }
  wine_vkFreeCommandBuffers(&client_device,pools[round],3,buffers);
  for(unsigned i=0;i<3;i++)assert(!client_buffers[i].obj.unix_handle);
  assert(live==2);
 }
 /* A failure after one success frees that success through the original pool. */
 info.commandPool=pools[0];fail_driver=driver_calls+2;
 assert(wine_vkAllocateCommandBuffers(&client_device,&info,buffers)==VK_ERROR_OUT_OF_DEVICE_MEMORY);
 assert(live==2);for(unsigned i=0;i<3;i++)assert(!client_buffers[i].obj.unix_handle);
 fail_driver=0;fail_alloc=allocations+2;
 assert(wine_vkAllocateCommandBuffers(&client_device,&info,buffers)==VK_ERROR_OUT_OF_HOST_MEMORY);
 assert(live==2);for(unsigned i=0;i<3;i++)assert(!client_buffers[i].obj.unix_handle);
 fail_alloc=0;
 wine_vkFreeCommandBuffers(&client_device,pools[0],3,buffers);assert(live==2);
 buffers[0]=NULL;wine_vkFreeCommandBuffers(&client_device,pools[0],1,buffers);
 for(unsigned i=0;i<2;i++)wine_vkDestroyCommandPool(&client_device,pools[i],NULL);
 assert(!live && inserted==removed && freed==8);
 puts("PASS native Wine pool membership, primary/secondary, reuse and partial failure cleanup");return 0;
}
'''
(out / 'fixture.c').write_text(fixture)
flags = ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-missing-braces', '-D__WINESRC__', '-DWINE_UNIX_LIB', '-D_WIN64',
         '-I' + str(out), '-I' + str(build / 'include'), '-I' + str(source / 'include'), '-I' + str(d)]
commands = []
for name, extra in [('host', []), ('sanitize', ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'])]:
    for command in [['systemd-run', '--user', '--scope', '-q', '-p', 'MemoryMax=8G', 'cc', *flags, *extra, str(out / 'fixture.c'), '-o', str(out / name)], [str(out / name)]]:
        result = subprocess.run(command, text=True, capture_output=True, timeout=60)
        commands.append(dict(command=command, exit_code=result.returncode, stdout=result.stdout, stderr=result.stderr))
        assert not result.returncode, result.stdout + result.stderr
# Missing membership must be detected by the actual lifecycle assertions.
negative = fixture.replace('((struct wine_cmd_buffer *)buffer)->pool = pool;', '(void)pool;')
assert negative != fixture
(out / 'negative.c').write_text(negative)
command = ['systemd-run', '--user', '--scope', '-q', '-p', 'MemoryMax=8G', 'cc', *flags,
           str(out / 'negative.c'), '-o', str(out / 'negative')]
result = subprocess.run(command, text=True, capture_output=True, timeout=60)
assert not result.returncode, result.stderr
commands.append(dict(command=command, exit_code=result.returncode, stdout=result.stdout, stderr=result.stderr))
command = [str(out / 'negative')]
result = subprocess.run(command, text=True, capture_output=True, timeout=60,
                        preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
assert result.returncode == -6 and 'b->pool==' in result.stderr, result.stdout + result.stderr
commands.append(dict(command=command, exit_code=result.returncode, expected_failure=True,
                     stdout=result.stdout, stderr=result.stderr))
receipt = dict(status='pass', console_accessed=False, actual_staged_lifecycle_bodies=True, commands=commands,
               sha256={str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in [repo/'tools/stage_vk_replay_lifecycle.py', Path(__file__), d/'vulkan.c', d/'vulkan_private.h', out/'fixture.c']})
(out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print('PASS ' + str(out / 'receipt.json'))
