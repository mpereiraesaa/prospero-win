/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Actual Unix adapter and owned stream, with a controlled native Vulkan driver. */
#define _GNU_SOURCE 1
#include <assert.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#define pw_vk_replay_create intercepted_replay_create
#include "pw_vk_batch_unix.c"
#undef pw_vk_replay_create
struct pw_vk_replay *pw_vk_replay_create(unsigned,size_t,pw_vk_replay_fn);

static pthread_mutex_t driver_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t driver_cond = PTHREAD_COND_INITIALIZER;
static unsigned blocked[3], entered[3], finished[3], next_value[3], active_pool[2];
static struct wine_cmd_pool pools[2];
static struct wine_cmd_buffer buffers[3];
static struct vulkan_device device;
static struct VkCommandBuffer_T *clients;
static struct vk_command_pool *client_pools;
static unsigned char *batch_memory;
static unsigned fanout_mode,expected_fanout,init_pause,init_entered,init_release;
struct pw_vk_replay *intercepted_replay_create(unsigned count,size_t capacity,pw_vk_replay_fn execute)
{
 if(init_pause){
  pthread_mutex_lock(&driver_mutex);init_entered=1;pthread_cond_broadcast(&driver_cond);
  while(!init_release)pthread_cond_wait(&driver_cond,&driver_mutex);
  pthread_mutex_unlock(&driver_mutex);
 }
 return pw_vk_replay_create(count,capacity,execute);
}
static void *initialize_thread(void *unused)
{
 struct pw_vk_batch_params params={0};(void)unused;
 params.version=PW_VK_BATCH_ASYNC_VERSION;params.code=unix_count+1;
 assert(!pw_vk_batch_unix(&params));return NULL;
}
static unsigned submitted_version=PW_VK_BATCH_ASYNC_VERSION;

/* This fixture covers actual manual codecs. Generated codecs have their own
 * actual-PE/native round-trip lab and classifier tests; no fake decoder success. */
int pw_vk_generated_decode(unsigned code,const void *wire,size_t bytes,void *arena,size_t capacity,void **params)
{
 (void)code;(void)wire;(void)bytes;(void)arena;(void)capacity;(void)params;return 0;
}
NTSTATUS pw_vk_batch_dispatch_native(unsigned code,void *params)
{
 (void)code;(void)params;assert(!"unexpected generated dispatch");return STATUS_UNSUCCESSFUL;
}
NTSTATUS pw_vk_batch_dispatch(unsigned code,void *params)
{
 (void)params;
 if(code==unix_vkCreateCommandPool || code==unix_vkAllocateCommandBuffers){assert(pw_vk_async_pool_fanout_count()==expected_fanout);return STATUS_SUCCESS;}
 assert(code==unix_vkGetFenceStatus);return STATUS_SUCCESS;
}
static void driver_push(VkCommandBuffer host,VkPipelineLayout layout,VkShaderStageFlags stages,
                        uint32_t offset,uint32_t bytes,const void *data)
{
 unsigned id=(uintptr_t)host-1,pool=id==2?1:0,value;
 assert(id<3 && layout==17 && stages==1 && offset==0 && bytes==sizeof(value));
 pthread_mutex_lock(&driver_mutex);
 assert(!active_pool[pool]);active_pool[pool]++;
 entered[id]++;pthread_cond_broadcast(&driver_cond);
 while(blocked[id])pthread_cond_wait(&driver_cond,&driver_mutex);
 /* Read after unblock: the producer has already overwritten every input byte. */
 memcpy(&value,data,sizeof(value));assert(value==next_value[id]++);
 active_pool[pool]--;finished[id]++;pthread_cond_broadcast(&driver_cond);
 pthread_mutex_unlock(&driver_mutex);
}
static void await_entered(unsigned id,unsigned target)
{
 struct timespec deadline;clock_gettime(CLOCK_REALTIME,&deadline);deadline.tv_sec+=5;
 pthread_mutex_lock(&driver_mutex);
 while(entered[id]<target)assert(!pthread_cond_timedwait(&driver_cond,&driver_mutex,&deadline));
 pthread_mutex_unlock(&driver_mutex);
}
static void release_driver(unsigned id)
{
 pthread_mutex_lock(&driver_mutex);blocked[id]=0;pthread_cond_broadcast(&driver_cond);pthread_mutex_unlock(&driver_mutex);
}
static void assert_not_entered(unsigned id)
{
 struct timespec deadline;clock_gettime(CLOCK_REALTIME,&deadline);deadline.tv_nsec+=100000000;
 if(deadline.tv_nsec>=1000000000){deadline.tv_sec++;deadline.tv_nsec-=1000000000;}
 pthread_mutex_lock(&driver_mutex);
 while(!entered[id]){int status=pthread_cond_timedwait(&driver_cond,&driver_mutex,&deadline);if(status==ETIMEDOUT)break;assert(!status);}
 assert(!entered[id]);pthread_mutex_unlock(&driver_mutex);
}
static unsigned waiter_started,waiter_done;
static void *lifecycle_waiter(void *mode)
{
 pthread_mutex_lock(&driver_mutex);waiter_started=1;pthread_cond_broadcast(&driver_cond);pthread_mutex_unlock(&driver_mutex);
 if(mode)pw_vk_async_forget_buffer(&clients[1]);
 else pw_vk_async_wait_pool((VkCommandPool)(uintptr_t)&client_pools[0]);
 pthread_mutex_lock(&driver_mutex);waiter_done=1;pthread_cond_broadcast(&driver_cond);pthread_mutex_unlock(&driver_mutex);
 return NULL;
}
static void assert_waiter_blocked(void)
{
 struct timespec deadline;clock_gettime(CLOCK_REALTIME,&deadline);deadline.tv_sec+=5;
 pthread_mutex_lock(&driver_mutex);
 while(!waiter_started)assert(!pthread_cond_timedwait(&driver_cond,&driver_mutex,&deadline));
 clock_gettime(CLOCK_REALTIME,&deadline);deadline.tv_nsec+=100000000;
 if(deadline.tv_nsec>=1000000000){deadline.tv_sec++;deadline.tv_nsec-=1000000000;}
 while(!waiter_done){int status=pthread_cond_timedwait(&driver_cond,&driver_mutex,&deadline);if(status==ETIMEDOUT)break;assert(!status);}
 assert(!waiter_done);pthread_mutex_unlock(&driver_mutex);
}
static void submit_records(unsigned id,unsigned first,unsigned count)
{
 unsigned char payload[64],arena[4096];size_t bytes,records,payload_bytes;
 struct pw_vk_stream_registry registry;struct pw_vk_stream stream={0};
 struct pw_vk_batch_params params={0};
 pw_vk_stream_registry_init(&registry);assert(!pw_vk_stream_register(&registry,&stream,arena,sizeof(arena)));
 for(unsigned i=0;i<count;i++){
  unsigned value=first+i;
  assert(pw_vk_wire_push_constants(payload,sizeof(payload),(uintptr_t)&clients[id],17,1,0,sizeof(value),&value,&payload_bytes));
  assert(!pw_vk_stream_append(&registry,&stream,PW_VK_PUSH_CONSTANTS,payload,payload_bytes));
 }
 assert(!pw_vk_stream_collect(&registry,batch_memory,4096,&bytes,&records));assert(records==count);
 params.version=submitted_version;params.batch=(uintptr_t)batch_memory;params.bytes=bytes;
 params.code=submitted_version==PW_VK_BATCH_ASYNC_VERSION?unix_count+1:unix_vkGetFenceStatus;
 assert(pw_vk_batch_unix(&params)==STATUS_SUCCESS && params.status==STATUS_SUCCESS);
 memset(batch_memory,0xcc,bytes);memset(arena,0xcc,sizeof(arena));memset(payload,0xcc,sizeof(payload));
}
static void setup(void)
{
 void *memory=mmap(NULL,16384,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
 assert(memory!=MAP_FAILED && (uintptr_t)memory+16384<=UINT32_MAX);
 clients=memory;client_pools=(void *)((unsigned char *)memory+4096);batch_memory=(unsigned char *)memory+8192;
 device.p_vkCmdPushConstants=driver_push;
 for(unsigned i=0;i<2;i++){client_pools[i].obj.unix_handle=(uintptr_t)&pools[i];pools[i].slot_count=1;pools[i].slot_limit=1;}
 for(unsigned i=0;i<3;i++){
  clients[i].obj.unix_handle=(uintptr_t)&buffers[i];buffers[i].obj.device=&device;
  buffers[i].obj.host.command_buffer=(VkCommandBuffer)(uintptr_t)(i+1);buffers[i].pool=&pools[i==2?1:0];
 }
 assert(!setenv("PW_VK_REPLAY_THREADS","2",1));assert(!setenv("PW_VK_BATCH_STATS","1",1));
}
static void fatal_failure(void)
{
 unsigned char invalid[PW_VK_STREAM_HEADER]={0};uint64_t ticket;
 pthread_once(&worker_once,initialize_workers);assert(workers);
 buffers[0].replay_lane=pw_vk_replay_lane_create(workers,(uintptr_t)buffers[0].pool,&buffers[0]);
 assert(buffers[0].replay_lane);
 assert(!pw_vk_replay_enqueue(buffers[0].replay_lane,invalid,sizeof(invalid),&ticket));
 pw_vk_async_wait_buffer(&clients[0]);
 assert(!"failed worker returned through lifecycle hook");
}
int main(int argc,char **argv)
{
 struct pw_vk_replay_stats stats;pthread_t waiter;(void)argv;alarm(15);setup();
 if(argc>1 && !strcmp(argv[1],"init-race")){
  pthread_t initializer;struct timespec deadline;
  init_pause=1;assert(!pthread_create(&initializer,NULL,initialize_thread,NULL));
  clock_gettime(CLOCK_REALTIME,&deadline);deadline.tv_sec+=5;
  pthread_mutex_lock(&driver_mutex);
  while(!init_entered)assert(!pthread_cond_timedwait(&driver_cond,&driver_mutex,&deadline));
  pthread_mutex_unlock(&driver_mutex);
  assert(!pthread_create(&waiter,NULL,lifecycle_waiter,NULL));assert_waiter_blocked();
  pthread_mutex_lock(&driver_mutex);init_release=1;pthread_cond_broadcast(&driver_cond);pthread_mutex_unlock(&driver_mutex);
  assert(!pthread_join(initializer,NULL));assert(!pthread_join(waiter,NULL));assert(waiter_done);
  stop_workers();munmap(clients,16384);
  puts("PASS raw lifecycle hook cannot observe partially initialized workers");return 0;
 }
 if(argc>1 && !strcmp(argv[1],"legacy")){
  submitted_version=PW_VK_BATCH_VERSION;submit_records(0,0,3);
  assert(finished[0]==3 && !workers);
  {struct pw_vk_batch_params bad={0};bad.version=PW_VK_BATCH_VERSION;bad.code=unix_count+1;
   assert(pw_vk_batch_unix(&bad)==STATUS_INVALID_PARAMETER);}
  munmap(clients,16384);
  puts("PASS legacy v2 remains synchronous and does not start workers");return 0;
 }
 if(argc>1 && !strcmp(argv[1],"trace-bounds")){
  for(unsigned i=0;i<10;i++){submit_records(0,40*i,40);pw_vk_async_wait_buffer(&clients[0]);}
  assert(finished[0]==400);pw_vk_async_forget_buffer(&clients[0]);stop_workers();munmap(clients,16384);
  puts("PASS trace per-job bounds: ten jobs of forty records");return 0;
 }
 if(argc>1 && !strcmp(argv[1],"fanout")){
  uint32_t *fields=(void *)(batch_memory+4096),*info=fields+16;
  struct pw_vk_batch_params params={0};
  fanout_mode=1;pools[0].slot_count=pools[0].slot_limit=2;buffers[2].pool=&pools[0];buffers[2].pool_slot=1;
  assert(!setenv("PW_VK_REPLAY_POOL_FANOUT","1",1));
  fields[1]=(uintptr_t)info;info[0]=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  params.version=PW_VK_BATCH_ASYNC_VERSION;params.code=unix_vkCreateCommandPool;params.args=(uintptr_t)fields;
  expected_fanout=2;assert(!pw_vk_batch_unix(&params));assert(pw_vk_async_pool_fanout_count()==1);
  info[1]=1;expected_fanout=1;assert(!pw_vk_batch_unix(&params));info[1]=0;
  fields[2]=1;assert(!pw_vk_batch_unix(&params));fields[2]=0;
  params.version=PW_VK_BATCH_VERSION;assert(!pw_vk_batch_unix(&params));
  params.version=PW_VK_BATCH_ASYNC_VERSION;params.code=unix_vkAllocateCommandBuffers;expected_fanout=2;
  assert(!pw_vk_batch_unix(&params));assert(pw_vk_async_pool_fanout_count()==1);
 }else if(argc>1){fatal_failure();return 2;}

 blocked[0]=blocked[2]=1;
 submit_records(0,0,3);await_entered(0,1);
 submit_records(2,0,2);await_entered(2,1);
 pw_vk_replay_get_stats(workers,&stats);assert(stats.workers==2 && stats.active==2 && stats.peak_active==2);
 /* Both workers are blocked; a second lane from pool 0 cannot overlap lane 0. */
 submit_records(1,0,2);release_driver(2);pw_vk_async_wait_buffer(&clients[2]);
 assert(finished[2]==2);assert_not_entered(1);
 if(fanout_mode)pw_vk_async_wait_buffer_pool(&clients[2]); /* does not wait slot 0 */
 /* Targeted submit prerequisite completed while the unrelated pool is blocked. */
 assert(!finished[0]);
 assert(!pthread_create(&waiter,NULL,lifecycle_waiter,NULL));assert_waiter_blocked();
 release_driver(0);assert(!pthread_join(waiter,NULL));assert(waiter_done);
 pw_vk_async_wait_buffer_pool(&clients[1]);assert(finished[0]==3 && finished[1]==2);
 pw_vk_async_wait_pool((VkCommandPool)(uintptr_t)&client_pools[0]);
 for(unsigned i=0;i<3;i++){pw_vk_async_forget_buffer(&clients[i]);assert(!buffers[i].replay_lane);}
 /* A freed lane can be recreated for the same native object/pool identity. */
 blocked[1]=1;submit_records(1,2,2);await_entered(1,3);
 waiter_started=waiter_done=0;
 assert(!pthread_create(&waiter,NULL,lifecycle_waiter,(void *)1));assert_waiter_blocked();
 assert(buffers[1].replay_lane);release_driver(1);assert(!pthread_join(waiter,NULL));
 assert(finished[1]==4 && !buffers[1].replay_lane);
 pw_vk_replay_get_stats(workers,&stats);
 assert(stats.submitted==4 && stats.completed==4 && !stats.owned_bytes && !stats.callback_error);
 assert(stats.worker_jobs[0] && stats.worker_jobs[1] && stats.peak_active==2);
 if(fanout_mode){
  blocked[2]=1;submit_records(2,2,1);await_entered(2,3);
  waiter_started=waiter_done=0;assert(!pthread_create(&waiter,NULL,lifecycle_waiter,NULL));assert_waiter_blocked();
  release_driver(2);assert(!pthread_join(waiter,NULL));assert(finished[2]==3);
  pw_vk_async_forget_buffer(&clients[2]);
 }
 report_workers();stop_workers();munmap(clients,16384);
 puts("PASS async adapter: two pools overlap, same-pool exclusion, owned input, ordered replay, scoped waits, lane teardown and stats");
 return 0;
}
