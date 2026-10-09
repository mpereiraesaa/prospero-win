/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <sched.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include "wine/ps5/pw_vk_replay.h"
/* Deterministic partial worker-start failure, without altering production API. */
static unsigned create_calls,fail_create;
int __real_pthread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
int __wrap_pthread_create(pthread_t *t,const pthread_attr_t *a,void *(*fn)(void *),void *arg)
{
 if(++create_calls==fail_create)return EAGAIN;
 return __real_pthread_create(t,a,fn,arg);
}
struct fixture {
 pthread_mutex_t mutex;
 pthread_cond_t changed;
 pthread_t main;
 unsigned active[3],total,peak;
};
struct context {
 struct fixture *f;
 unsigned pool,entered,last,release;
};
struct payload {unsigned sequence,magic;int fail;unsigned char padding[84];};
static int execute(void *arg,const void *data,size_t bytes)
{
 struct context *c=arg;struct fixture *f=c->f;struct payload p;
 assert(bytes==sizeof(p));memcpy(&p,data,sizeof(p));
 assert(p.magic==0x12345678);assert(!pthread_equal(pthread_self(),f->main));
 pthread_mutex_lock(&f->mutex);
 assert(!f->active[c->pool]++);assert(p.sequence==c->last+1);
 ++c->entered;++f->total;if(f->total>f->peak)f->peak=f->total;
 pthread_cond_broadcast(&f->changed);
 while(!c->release)pthread_cond_wait(&f->changed,&f->mutex);
 c->last=p.sequence;--f->active[c->pool];--f->total;
 pthread_cond_broadcast(&f->changed);pthread_mutex_unlock(&f->mutex);return p.fail;
}
static void init(struct fixture *f)
{
 memset(f,0,sizeof(*f));assert(!pthread_mutex_init(&f->mutex,NULL));assert(!pthread_cond_init(&f->changed,NULL));f->main=pthread_self();
}
static void finish(struct fixture *f)
{
 assert(!pthread_cond_destroy(&f->changed));assert(!pthread_mutex_destroy(&f->mutex));
}
static void entered(struct context *c,unsigned n)
{
 struct timespec deadline;struct fixture *f=c->f;
 assert(!clock_gettime(CLOCK_REALTIME,&deadline));deadline.tv_sec+=5;
 pthread_mutex_lock(&f->mutex);
 while(c->entered<n)assert(!pthread_cond_timedwait(&f->changed,&f->mutex,&deadline));
 pthread_mutex_unlock(&f->mutex);
}
static void release(struct context *c)
{
 pthread_mutex_lock(&c->f->mutex);c->release=1;pthread_cond_broadcast(&c->f->changed);pthread_mutex_unlock(&c->f->mutex);
}
static uint64_t append(struct pw_vk_replay_lane *lane,unsigned n,int fail)
{
 struct payload p={.sequence=n,.magic=0x12345678,.fail=fail};uint64_t ticket=0;
 assert(pw_vk_replay_enqueue(lane,&p,sizeof(p),&ticket)==0);memset(&p,0xee,sizeof(p));return ticket;
}
static void parallel_and_ordered(void)
{
 struct fixture f;struct pw_vk_replay *s;struct pw_vk_replay_lane *a,*b,*c;struct pw_vk_replay_stats stats;
 init(&f);struct context ca={&f,1,0,0,0},cb={&f,1,0,0,1},cc={&f,2,0,0,0};
 s=pw_vk_replay_create(2,1024,execute);assert(s);
 a=pw_vk_replay_lane_create(s,11,&ca);b=pw_vk_replay_lane_create(s,11,&cb);c=pw_vk_replay_lane_create(s,22,&cc);assert(a&&b&&c);
 assert(append(a,1,0)==1);entered(&ca,1);
 assert(append(b,1,0)==1);assert(append(a,2,0)==2);assert(append(c,1,0)==1);entered(&cc,1);
 pthread_mutex_lock(&f.mutex);assert(!cb.entered);assert(ca.entered==1);assert(f.total==2);pthread_mutex_unlock(&f.mutex);
 release(&cc);assert(!pw_vk_replay_wait(c,1));
 /* A targeted completion never waits for an unrelated blocked pool. */
 assert(!pw_vk_replay_wait_pool(s,22));assert(pw_vk_replay_wait(c,2)==PW_VK_REPLAY_INVALID);
 pthread_mutex_lock(&f.mutex);assert(f.total==1 && !ca.last && !cb.entered);pthread_mutex_unlock(&f.mutex);
 release(&ca);assert(!pw_vk_replay_wait(a,2));assert(!pw_vk_replay_wait_pool(s,11));
 for(unsigned i=0;i<1000;i++){
  append(a,i+3,0);append(b,i+2,0);append(c,i+2,0);
 }
 assert(pw_vk_replay_marker(a)==1002);assert(!pw_vk_replay_wait_all(s));
 pw_vk_replay_get_stats(s,&stats);assert(stats.workers==2 && stats.peak_active==2);
 assert(stats.submitted==3004 && stats.completed==3004 && !stats.owned_bytes && stats.peak_owned_bytes<=1024);
 assert(stats.worker_jobs[0] && stats.worker_jobs[1]);assert(f.peak==2);
 assert(!pw_vk_replay_lane_drop(a));assert(!pw_vk_replay_lane_drop(b));assert(!pw_vk_replay_lane_drop(c));
 /* Reusing a released pool identity starts a new lane generation. */
 ca.last=0;a=pw_vk_replay_lane_create(s,11,&ca);assert(a);assert(append(a,1,0)==1);assert(!pw_vk_replay_lane_drop(a));
 pw_vk_replay_destroy(s);finish(&f);
}
struct admission {struct pw_vk_replay_lane *lane;uint64_t ticket;};
static void *admit(void *arg)
{
 struct admission *a=arg;a->ticket=append(a->lane,3,0);return NULL;
}
static void backpressure(void)
{
 struct fixture f;struct pw_vk_replay *s;struct pw_vk_replay_lane *lane;struct pw_vk_replay_stats stats;
 struct timespec start,now;pthread_t producer;
 init(&f);struct context c={&f,1,0,0,0};
 /* At most two 96-byte payload jobs fit on both 32/64-bit hosts, including headers. */
 s=pw_vk_replay_create(2,288,execute);assert(s);lane=pw_vk_replay_lane_create(s,1,&c);assert(lane);
 append(lane,1,0);entered(&c,1);append(lane,2,0);
 struct admission a={lane,0};assert(!pthread_create(&producer,NULL,admit,&a));
 assert(!clock_gettime(CLOCK_MONOTONIC,&start));
 do{
  pw_vk_replay_get_stats(s,&stats);assert(!clock_gettime(CLOCK_MONOTONIC,&now));
  assert(now.tv_sec-start.tv_sec<5);sched_yield();
 }while(!stats.capacity_waits);
 assert(stats.submitted==2);release(&c);assert(!pthread_join(producer,NULL));assert(a.ticket==3);
 assert(!pw_vk_replay_wait_all(s));pw_vk_replay_get_stats(s,&stats);
 assert(stats.completed==3 && !stats.owned_bytes && stats.peak_owned_bytes<=288);
 assert(!pw_vk_replay_lane_drop(lane));pw_vk_replay_destroy(s);finish(&f);
}
static void errors(void)
{
 struct fixture f;struct pw_vk_replay *s;struct pw_vk_replay_lane *a;struct pw_vk_replay_stats stats;uint64_t ticket=99;
 init(&f);struct context ca={&f,1,0,0,1};
 assert(!pw_vk_replay_create(0,1024,execute));assert(!pw_vk_replay_create(9,1024,execute));assert(!pw_vk_replay_create(2,1,execute));assert(!pw_vk_replay_create(2,1024,NULL));
 fail_create=create_calls+2;assert(!pw_vk_replay_create(2,1024,execute));fail_create=0;
 s=pw_vk_replay_create(1,1024,execute);assert(s);assert(!pw_vk_replay_lane_create(s,0,&ca));
 a=pw_vk_replay_lane_create(s,1,&ca);assert(a);
 assert(pw_vk_replay_enqueue(a,NULL,1,&ticket)==PW_VK_REPLAY_INVALID);
 assert(pw_vk_replay_enqueue(a,"",SIZE_MAX,&ticket)==PW_VK_REPLAY_INVALID);
 assert(pw_vk_replay_enqueue(a,"",1024,&ticket)==PW_VK_REPLAY_INVALID);assert(ticket==99);
 assert(pw_vk_replay_marker(a)==0);assert(!pw_vk_replay_wait(a,0));assert(pw_vk_replay_wait_pool(s,0)==PW_VK_REPLAY_INVALID);
 append(a,1,17);assert(pw_vk_replay_wait(a,1)==PW_VK_REPLAY_FAILED);
 assert(pw_vk_replay_wait_all(s)==PW_VK_REPLAY_FAILED);
 assert(pw_vk_replay_enqueue(a,"",0,&ticket)==PW_VK_REPLAY_FAILED);
 assert(!pw_vk_replay_lane_create(s,2,&ca));assert(pw_vk_replay_lane_drop(a)==PW_VK_REPLAY_FAILED);
 pw_vk_replay_get_stats(s,&stats);assert(stats.callback_error==17 && stats.completed==1);
 pw_vk_replay_destroy(s);finish(&f);
}
static unsigned occurrences(const char *text,const char *needle)
{
 unsigned count=0;while((text=strstr(text,needle))){++count;text+=strlen(needle);}return count;
}
static void startup_trace(void)
{
 unsigned enabled,i;char text[16384];
 for(enabled=0;enabled<2;enabled++){
  struct fixture f;struct pw_vk_replay *s;struct pw_vk_replay_lane *lane;
  FILE *capture=tmpfile();int saved=dup(STDERR_FILENO);size_t bytes;
  assert(capture && saved>=0);assert(!setenv("PW_VK_REPLAY_TRACE",enabled?"1":"0",1));
  fflush(stderr);assert(dup2(fileno(capture),STDERR_FILENO)>=0);
  init(&f);struct context c={&f,1,0,0,1};
  s=pw_vk_replay_create(2,4096,execute);assert(s);
  lane=pw_vk_replay_lane_create(s,1,&c);assert(lane);
  for(i=1;i<=20;i++)append(lane,i,0);
  assert(!pw_vk_replay_wait_all(s));assert(!pw_vk_replay_lane_drop(lane));
  pw_vk_replay_destroy(s);finish(&f);fflush(stderr);
  assert(dup2(saved,STDERR_FILENO)>=0);close(saved);rewind(capture);
  bytes=fread(text,1,sizeof(text)-1,capture);assert(!ferror(capture));text[bytes]=0;fclose(capture);
  if(!enabled)assert(!bytes);
  else{
   assert(occurrences(text,"event=create_begin ")==1);
   assert(occurrences(text,"event=create_end ")==1);
   assert(occurrences(text,"event=pthread_create_begin ")==2);
   assert(occurrences(text,"event=pthread_create_end ")==2);
   assert(occurrences(text,"event=worker_enter ")==2);
   assert(occurrences(text,"event=job_begin ")==8);
   assert(occurrences(text,"event=job_end ")==8);
  }
 }
 assert(!unsetenv("PW_VK_REPLAY_TRACE"));
}
int main(void)
{
 startup_trace();parallel_and_ordered();backpressure();errors();puts("Vulkan replay scheduler: independent overlap, pool exclusion, owned order, scoped waits and failure passed");return 0;
}
