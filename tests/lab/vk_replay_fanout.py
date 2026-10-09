#!/usr/bin/env python3
"""Exercise actual staged native pool bodies, bounded fanout and regeneration."""
import argparse
import hashlib
import io
import json
import logging
import os
from pathlib import Path
import resource
import runpy
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', required=True)
p.add_argument('--build', required=True)
p.add_argument('--vk-xml', required=True)
p.add_argument('--video-xml', required=True)
p.add_argument('--output', required=True)
a = p.parse_args()
repo = Path(__file__).resolve().parents[2]
source, build, out = (Path(v).resolve() for v in (a.source, a.build, a.output))
vk_xml, video_xml = str(Path(a.vk_xml).resolve()), str(Path(a.video_xml).resolve())
out.mkdir(parents=True, exist_ok=True)
d = source / 'dlls/winevulkan'
life = runpy.run_path(str(repo / 'tools/stage_vk_replay_lifecycle.py'))
barrier = runpy.run_path(str(repo / 'tools/stage_vk_replay_barriers.py'))
fanout = runpy.run_path(str(repo / 'tools/stage_vk_replay_fanout.py'))
h, c = life['transform']((d / 'vulkan_private.h').read_text(), (d / 'vulkan.c').read_text())
inputs = barrier['transform'](h, c, (d / 'vulkan_thunks.c').read_text(), (d / 'make_vulkan').read_text())
header, code, thunks, generator = fanout['transform'](*inputs)
assert (header, code, thunks, generator) == fanout['transform'](*inputs)
try:
    fanout['transform'](header, code, thunks, generator)
except AssertionError:
    pass
else:
    raise AssertionError('duplicate stage accepted')
(out / 'vulkan_private.h').write_text(header)
(out / 'pw_vk_async.h').write_bytes((repo / 'wine/ps5/vulkan/pw_vk_async.h').read_bytes())
(out / 'make_vulkan').write_text(generator)
module = runpy.run_path(str(out / 'make_vulkan'))
module['LOGGER'].setLevel(logging.WARNING)
os.chdir(d)
g = module['Generator'](vk_xml, video_xml)
buffer = io.StringIO()
g.generate_thunks_c(buffer)
assert buffer.getvalue() == thunks, 'fanout generator mismatch'


def body(name):
    start = code.index(name + '(')
    start = code.rfind('\n', 0, start) + 1
    return code[start:code.index('\n}\n', start) + 3]


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
static unsigned limit=3,allocations,fail_alloc,live,inserted,removed,driver_calls,fail_driver,freed;
static unsigned creates,fail_create,destroys,waits,forgets,resets,trims,khr_trims;
static VkResult create_failure=VK_ERROR_OUT_OF_DEVICE_MEMORY;
static VkCommandPool reset_fail;
static VkCommandPoolCreateFlags expected_flags;
static const void *expected_chain;
static void *tracked_calloc(size_t n,size_t size)
{if(++allocations==fail_alloc)return NULL;void *p=calloc(n,size);if(p)++live;return p;}
static void tracked_free(void *p){if(p){assert(live);--live;}free(p);}
unsigned pw_vk_async_pool_fanout_count(void){return limit;}
void pw_vk_async_wait_pool(VkCommandPool p){assert(p);waits++;}
void pw_vk_async_forget_buffer(VkCommandBuffer p){assert(vulkan_command_buffer_from_handle(p));forgets++;}
#define calloc tracked_calloc
#define free tracked_free
'''
for name in ('pw_vk_pool_report', 'pw_vk_pool_select', 'wine_vkResetCommandPool', 'wine_vkTrimCommandPool', 'wine_vkTrimCommandPoolKHR',
             'wine_vk_free_command_buffers', 'wine_vkAllocateCommandBuffers', 'wine_vkFreeCommandBuffers',
             'wine_vkCreateCommandPool', 'wine_vkDestroyCommandPool'):
    fixture += body(name)
fixture += r'''
#undef calloc
#undef free
static void insert(struct vulkan_instance *i,struct vulkan_object *o)
{(void)i;assert(o->client_handle && o->host_handle);++inserted;}
static void remove_object(struct vulkan_instance *i,struct vulkan_object *o)
{(void)i;assert(o->client_handle && o->host_handle);++removed;}
static VkResult create_pool(VkDevice d,const VkCommandPoolCreateInfo *info,const VkAllocationCallbacks *a,VkCommandPool *p)
{
 (void)d;assert(!a);assert(info->flags==expected_flags && info->queueFamilyIndex==7);
 assert(info->pNext==expected_chain);
 if(++creates==fail_create)return create_failure;
 *p=100+creates;return VK_SUCCESS;
}
static void destroy_pool(VkDevice d,VkCommandPool p,const VkAllocationCallbacks *a)
{(void)d;assert(!a && p>=100);destroys++;}
static VkResult reset_pool(VkDevice d,VkCommandPool p,VkCommandPoolResetFlags flags)
{(void)d;assert(flags==VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT);resets++;return p==reset_fail?VK_ERROR_OUT_OF_HOST_MEMORY:VK_SUCCESS;}
static void trim_pool(VkDevice d,VkCommandPool p,VkCommandPoolTrimFlags flags)
{(void)d;assert(p>=100 && !flags);trims++;}
static void trim_pool_khr(VkDevice d,VkCommandPool p,VkCommandPoolTrimFlags flags)
{(void)d;assert(p>=100 && !flags);khr_trims++;}
static VkResult allocate(VkDevice d,const VkCommandBufferAllocateInfo *info,VkCommandBuffer *buffer)
{
 (void)d;assert(info->commandBufferCount==1 && info->commandPool>=100);
 assert(info->level==VK_COMMAND_BUFFER_LEVEL_PRIMARY || info->level==VK_COMMAND_BUFFER_LEVEL_SECONDARY);
 if(++driver_calls==fail_driver)return VK_ERROR_OUT_OF_DEVICE_MEMORY;
 *buffer=(VkCommandBuffer)(uintptr_t)(info->commandPool*1000+driver_calls);return VK_SUCCESS;
}
static void release_buffers(VkDevice d,VkCommandPool p,uint32_t count,const VkCommandBuffer *buffers)
{(void)d;assert(count==1);assert((uintptr_t)*buffers/1000==p);++freed;}
int main(void)
{
 assert(!setenv("PW_VK_BATCH_STATS","1",1));
 assert(!setenv("PW_VK_REPLAY_POOL_FANOUT","1",1));
 struct vulkan_instance instance={0};struct vulkan_physical_device physical={0};struct vulkan_device device={0};
 struct VkDevice_T client_device={0};struct vk_command_pool client_pool={0};
 struct VkCommandBuffer_T client_buffers[6]={{0}};VkCommandBuffer buffers[6];VkCommandPool pool;
 VkCommandPoolCreateInfo pool_info={0};VkCommandBufferAllocateInfo info={0};
 VkAllocationCallbacks allocator={0};VkBaseInStructure chain={0};
 instance.p_insert_object=insert;instance.p_remove_object=remove_object;
 physical.instance=&instance;device.physical_device=&physical;
 device.p_vkCreateCommandPool=create_pool;device.p_vkDestroyCommandPool=destroy_pool;
 device.p_vkAllocateCommandBuffers=allocate;device.p_vkFreeCommandBuffers=release_buffers;
 device.p_vkResetCommandPool=reset_pool;device.p_vkTrimCommandPool=trim_pool;device.p_vkTrimCommandPoolKHR=trim_pool_khr;
 client_device.obj.unix_handle=(uintptr_t)&device;
 pool_info.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;pool_info.queueFamilyIndex=7;
 info.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;info.commandBufferCount=6;
 for(unsigned round=0;round<7;round++){
  unsigned before_creates=creates,before_destroy=destroys;
  expected_flags=pool_info.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT|VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  expected_chain=pool_info.pNext=round==2?&chain:NULL;
  limit=round==1?1:3;fail_create=round==4||round==5?creates+2:0;
  create_failure=round==5?VK_ERROR_DEVICE_LOST:VK_ERROR_OUT_OF_DEVICE_MEMORY;
  pool=(uintptr_t)&client_pool;
  assert(!wine_vkCreateCommandPool(&client_device,&pool_info,round==3?&allocator:NULL,&pool));
  struct wine_cmd_pool *native=wine_cmd_pool_from_handle(pool);
  assert(native->slot_count==1 && native->host.command_pool==native->slots[0].host_handle);
  assert(native->slot_limit==(round==1||round==2||round==3?1:3));
  assert(client_pool.obj.unix_handle==(uintptr_t)native);
  info.commandPool=pool;info.level=round&1?VK_COMMAND_BUFFER_LEVEL_SECONDARY:VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  for(unsigned i=0;i<6;i++)buffers[i]=&client_buffers[i];
  if(round==5){
   assert(wine_vkAllocateCommandBuffers(&client_device,&info,buffers)==VK_ERROR_DEVICE_LOST);
   assert(live==1);for(unsigned i=0;i<6;i++)assert(!client_buffers[i].obj.unix_handle);
  }else{
   assert(!wine_vkAllocateCommandBuffers(&client_device,&info,buffers));
   unsigned slots=round==0||round==6?3:1;
   assert(native->slot_count==slots);
   for(unsigned i=0;i<6;i++){
    struct wine_cmd_buffer *b=wine_cmd_buffer_from_handle(buffers[i]);
    assert(b->pool==native && b->pool_slot<slots);
    assert((uintptr_t)b->obj.host.command_buffer/1000==native->slots[b->pool_slot].host_handle);
   }
   assert(client_pool.obj.unix_handle==(uintptr_t)native); /* alias insertion must preserve prefix */
   for(unsigned i=1;i<slots;i++)assert(native->slots[i].client_handle==native->obj.client_handle);
   unsigned old_resets=resets,old_trims=trims,old_khr=khr_trims;
   reset_fail=round==0?native->slots[1].host_handle:0;
   assert(wine_vkResetCommandPool(&client_device,pool,VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT)==(reset_fail?VK_ERROR_OUT_OF_HOST_MEMORY:VK_SUCCESS));
   wine_vkTrimCommandPool(&client_device,pool,0);wine_vkTrimCommandPoolKHR(&client_device,pool,0);
   assert(resets-old_resets==slots && trims-old_trims==slots && khr_trims-old_khr==slots);
   wine_vkFreeCommandBuffers(&client_device,pool,6,buffers);assert(live==1);
   /* Partial allocation cleanup uses each buffer's original physical slot. */
   fail_driver=driver_calls+3;
   assert(wine_vkAllocateCommandBuffers(&client_device,&info,buffers)==VK_ERROR_OUT_OF_DEVICE_MEMORY);
   fail_driver=0;assert(live==1);
   for(unsigned i=0;i<6;i++)assert(!client_buffers[i].obj.unix_handle);
   fail_alloc=allocations+3;
   assert(wine_vkAllocateCommandBuffers(&client_device,&info,buffers)==VK_ERROR_OUT_OF_HOST_MEMORY);
   fail_alloc=0;assert(live==1);
  }
  unsigned slots=native->slot_count;
  assert(creates-before_creates<=3);
  wine_vkDestroyCommandPool(&client_device,pool,NULL);
  assert(destroys-before_destroy==slots && !live && inserted==removed);
 }
 assert(freed==forgets && waits);
 puts("PASS actual native bounded pool fanout, aliases, reset/trim, primary/secondary, unknown chain, OOM and partial cleanup");
 return 0;
}
'''
(out / 'fixture.c').write_text(fixture)
flags = ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-missing-braces', '-Wno-missing-field-initializers',
         '-D__WINESRC__', '-DWINE_UNIX_LIB', '-D_WIN64', '-I'+str(out), '-I'+str(build/'include'),
         '-I'+str(source/'include'), '-I'+str(d)]
commands = []
for name, extra in [('host', []), ('san', ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie'])]:
    for command in [['cc', *flags, *extra, str(out/'fixture.c'), '-o', str(out/name)], [str(out/name)]]:
        result = subprocess.run(command, text=True, capture_output=True, timeout=60)
        commands.append(dict(command=command, exit_code=result.returncode, stdout=result.stdout, stderr=result.stderr))
        assert not result.returncode, result.stdout+result.stderr
        if len(command) == 1:
            lines = [line for line in result.stderr.splitlines() if line.startswith('PW_VK_REPLAY_POOL ')]
            assert sum('event=create ' in line for line in lines) == 7, lines
            assert sum('event=grow ' in line for line in lines) == 4, lines
            assert sum('event=degrade ' in line for line in lines) == 2, lines
            assert all('requested=1 ' in line for line in lines), lines
negative = fixture.replace('pool->slots[((struct wine_cmd_buffer *)buffer)->pool_slot].host_handle', 'pool->host.command_pool')
assert negative != fixture
(out/'negative.c').write_text(negative)
subprocess.run(['cc', *flags, str(out/'negative.c'), '-o', str(out/'negative')], check=True)
result = subprocess.run([str(out/'negative')], capture_output=True,
                        preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
assert result.returncode == -6 and b'Assertion' in result.stderr
receipt = dict(status='pass', actual_native_bodies=True, generator_equal=True, negative_exit=result.returncode,
               commands=commands, fixture_sha256=hashlib.sha256(fixture.encode()).hexdigest())
(out/'receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
print('PASS '+str(out/'receipt.json'))
