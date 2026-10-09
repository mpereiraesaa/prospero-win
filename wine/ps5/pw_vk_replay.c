/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_vk_replay.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
struct domain {
 struct domain *next;
 uint64_t key;
 unsigned references,busy;
};
struct pw_vk_replay_lane {
 struct pw_vk_replay_lane *next;
 struct pw_vk_replay *owner;
 struct domain *domain;
 void *context;
 uint64_t issued,done;
};
struct binding {struct pw_vk_replay_lane *lane;uint64_t ticket;};
struct job {
 struct job *next;
 size_t bytes,charge;
 uint64_t epoch;
 unsigned count,ordered;
 struct binding bindings[];
};

struct worker {
 pthread_t thread;
 struct pw_vk_replay *owner;
 unsigned index;
};
struct pw_vk_replay {
 pthread_mutex_t mutex;
 pthread_cond_t changed;
 struct worker workers[PW_VK_REPLAY_MAX_WORKERS];
 struct domain *domains;
 struct pw_vk_replay_lane *lanes;
 struct job *head,*tail;
 pw_vk_replay_fn execute;
 size_t limit;
 unsigned started,stopping,trace,trace_jobs,ordered_active;
 uint64_t ordered_issued,ordered_done;
 struct pw_vk_replay_stats stats;
};
static int failed(struct pw_vk_replay *s)
{
 return s->stopping || s->stats.callback_error;
}
/* A blocked multi-domain group reserves its place in every domain. A later
 * group must not pass it merely because one of those domains is idle. */
static int earlier_conflict(struct pw_vk_replay *s,const struct job *candidate)
{
 const struct job *prior;unsigned i,j;
 for(prior=s->head;prior!=candidate;prior=prior->next)
  for(i=0;i<prior->count;i++)for(j=0;j<candidate->count;j++)
   if(prior->bindings[i].lane->domain==candidate->bindings[j].lane->domain)return 1;
 return 0;
}
static void *worker_main(void *arg)
{
 struct worker *w=arg;struct pw_vk_replay *s=w->owner;
 if(s->trace)fprintf(stderr,"PW_VK_REPLAY_TRACE event=worker_enter worker=%u\n",w->index);
 pthread_mutex_lock(&s->mutex);
 for(;;){
  struct job *job=NULL,*previous=NULL;int result;unsigned traced,i;
  if(failed(s))break;
  if(!s->ordered_active)for(job=s->head;job;previous=job,job=job->next){
   /* Never pass an ordered epoch. It may start only at the queue head
    * after every earlier active job has completed. */
   if(job->ordered){if(previous || s->stats.active)job=NULL;break;}
   for(i=0;i<job->count;i++)if(job->bindings[i].lane->domain->busy)break;
   if(i==job->count && !earlier_conflict(s,job))break;
  }
  if(!job){pthread_cond_wait(&s->changed,&s->mutex);continue;}
  if(previous)previous->next=job->next;else s->head=job->next;
  if(s->tail==job)s->tail=previous;
  for(i=0;i<job->count;i++)job->bindings[i].lane->domain->busy=1;
  s->ordered_active=job->ordered;
  if(++s->stats.active>s->stats.peak_active)s->stats.peak_active=s->stats.active;
  traced=s->trace && s->trace_jobs<8;
  if(traced)++s->trace_jobs;
  pthread_mutex_unlock(&s->mutex);
  if(traced)fprintf(stderr,"PW_VK_REPLAY_TRACE event=job_begin worker=%u ticket=%llu bytes=%zu pool=%llu lanes=%u ordered=%u\n",w->index,(unsigned long long)(job->count?job->bindings[0].ticket:job->epoch),job->bytes,(unsigned long long)(job->count?job->bindings[0].lane->domain->key:0),job->count,job->ordered);
  result=s->execute(job->count?job->bindings[0].lane->context:NULL,job->bindings+job->count,job->bytes);
  if(traced)fprintf(stderr,"PW_VK_REPLAY_TRACE event=job_end worker=%u ticket=%llu result=%d\n",w->index,(unsigned long long)(job->count?job->bindings[0].ticket:job->epoch),result);
  pthread_mutex_lock(&s->mutex);
  if(result && !s->stats.callback_error)s->stats.callback_error=result;
  for(i=0;i<job->count;i++){
   job->bindings[i].lane->done=job->bindings[i].ticket;
   job->bindings[i].lane->domain->busy=0;
  }
  if(job->ordered){s->ordered_done=job->epoch;s->ordered_active=0;}
  --s->stats.active;++s->stats.completed;++s->stats.worker_jobs[w->index];
  s->stats.owned_bytes-=job->charge;free(job);
  pthread_cond_broadcast(&s->changed);
 }
 pthread_mutex_unlock(&s->mutex);return NULL;
}

struct pw_vk_replay *pw_vk_replay_create(unsigned workers,size_t limit,pw_vk_replay_fn execute)
{
 struct pw_vk_replay *s=NULL;unsigned i;pthread_attr_t attr;int status,destroy_status;
 const char *value=getenv("PW_VK_REPLAY_TRACE");int trace=value && !strcmp(value,"1");
 if(!workers || workers>PW_VK_REPLAY_MAX_WORKERS || limit<sizeof(struct job) || !execute)return NULL;
 if(trace)fprintf(stderr,"PW_VK_REPLAY_TRACE event=create_begin workers=%u limit=%zu stack=%u\n",workers,limit,PW_VK_REPLAY_WORKER_STACK);
 status=pthread_attr_init(&attr);
 if(status){if(trace)fprintf(stderr,"PW_VK_REPLAY_TRACE event=stack_setup phase=init result=%d\n",status);return NULL;}
 if(trace){
  size_t stack_bytes=0;int query_status=pthread_attr_getstacksize(&attr,&stack_bytes);
  fprintf(stderr,"PW_VK_REPLAY_TRACE event=stack_default bytes=%zu query_result=%d\n",stack_bytes,query_status);
 }
 status=pthread_attr_setstacksize(&attr,PW_VK_REPLAY_WORKER_STACK);
 if(trace)fprintf(stderr,"PW_VK_REPLAY_TRACE event=stack_setup phase=set bytes=%u result=%d\n",PW_VK_REPLAY_WORKER_STACK,status);
 if(status)goto done;
 if(!(s=calloc(1,sizeof(*s))))goto done;
 s->limit=limit;s->execute=execute;s->stats.workers=workers;s->trace=trace;
 if(pthread_mutex_init(&s->mutex,NULL)){free(s);s=NULL;goto done;}
 if(pthread_cond_init(&s->changed,NULL)){pthread_mutex_destroy(&s->mutex);free(s);s=NULL;goto done;}
 for(i=0;i<workers;i++){
  s->workers[i].owner=s;s->workers[i].index=i;
  if(trace)fprintf(stderr,"PW_VK_REPLAY_TRACE event=pthread_create_begin worker=%u\n",i);
  status=pthread_create(&s->workers[i].thread,&attr,worker_main,&s->workers[i]);
  if(trace)fprintf(stderr,"PW_VK_REPLAY_TRACE event=pthread_create_end worker=%u result=%d\n",i,status);
  if(status)goto done;
  ++s->started;
 }
 done:
 destroy_status=pthread_attr_destroy(&attr);
 if(trace)fprintf(stderr,"PW_VK_REPLAY_TRACE event=stack_setup phase=destroy result=%d\n",destroy_status);
 if(status || destroy_status){pw_vk_replay_destroy(s);return NULL;}
 if(trace && s)fprintf(stderr,"PW_VK_REPLAY_TRACE event=create_end workers=%u\n",s->started);
 return s;
}

struct pw_vk_replay_lane *pw_vk_replay_lane_create(struct pw_vk_replay *s,uint64_t pool,void *context)
{
 struct pw_vk_replay_lane *lane;struct domain *d;
 if(!s || !pool || !(lane=calloc(1,sizeof(*lane))))return NULL;
 pthread_mutex_lock(&s->mutex);
 if(failed(s)){pthread_mutex_unlock(&s->mutex);free(lane);return NULL;}
 for(d=s->domains;d && d->key!=pool;d=d->next){}
 if(!d){
  if(!(d=calloc(1,sizeof(*d)))){pthread_mutex_unlock(&s->mutex);free(lane);return NULL;}
  d->key=pool;d->next=s->domains;s->domains=d;
 }
 ++d->references;lane->owner=s;lane->domain=d;lane->context=context;
 lane->next=s->lanes;s->lanes=lane;
 pthread_mutex_unlock(&s->mutex);return lane;
}
static int enqueue_group(struct pw_vk_replay *s,struct pw_vk_replay_lane *const *lanes,
                         unsigned count,unsigned ordered,const void *data,size_t bytes,uint64_t *ticket)
{
 struct job *job;size_t charge;unsigned i,j;int status=PW_VK_REPLAY_OK;
 if(!s || count>PW_VK_REPLAY_MAX_GROUP_LANES || (count && !lanes) || (!count && !ordered) || ordered>1 ||
    (bytes && !data) || bytes>SIZE_MAX-sizeof(*job)-count*sizeof(struct binding))return PW_VK_REPLAY_INVALID;
 for(i=0;i<count;i++){
  if(!lanes[i] || lanes[i]->owner!=s)return PW_VK_REPLAY_INVALID;
  for(j=0;j<i;j++)if(lanes[j]==lanes[i])return PW_VK_REPLAY_INVALID;
 }
 charge=sizeof(*job)+count*sizeof(struct binding)+bytes;
 if(charge>s->limit)return PW_VK_REPLAY_INVALID;
 pthread_mutex_lock(&s->mutex);
 while(!failed(s) && charge>s->limit-s->stats.owned_bytes){
  ++s->stats.capacity_waits;pthread_cond_wait(&s->changed,&s->mutex);
 }
 if(failed(s))status=PW_VK_REPLAY_FAILED;
 else if(s->stats.submitted==UINT64_MAX || (ordered && s->ordered_issued==UINT64_MAX))status=PW_VK_REPLAY_EXHAUSTED;
 for(i=0;!status && i<count;i++)if(lanes[i]->issued==UINT64_MAX)status=PW_VK_REPLAY_EXHAUSTED;
 if(!status){
  if(!(job=malloc(charge)))status=PW_VK_REPLAY_MEMORY;
  else{
   job->bytes=bytes;job->charge=charge;job->count=count;job->ordered=ordered;job->next=NULL;
   job->epoch=ordered?++s->ordered_issued:0;
   for(i=0;i<count;i++){job->bindings[i].lane=lanes[i];job->bindings[i].ticket=++lanes[i]->issued;}
   if(bytes)memcpy(job->bindings+count,data,bytes);
   if(s->tail)s->tail->next=job;else s->head=job;s->tail=job;
   s->stats.owned_bytes+=charge;
   if(s->stats.owned_bytes>s->stats.peak_owned_bytes)s->stats.peak_owned_bytes=s->stats.owned_bytes;
   ++s->stats.submitted;if(ticket)*ticket=job->bindings[0].ticket;pthread_cond_broadcast(&s->changed);
  }
 }
 pthread_mutex_unlock(&s->mutex);return status;
}
int pw_vk_replay_enqueue(struct pw_vk_replay_lane *lane,const void *data,size_t bytes,uint64_t *ticket)
{
 if(!lane || !ticket)return PW_VK_REPLAY_INVALID;
 return enqueue_group(lane->owner,&lane,1,0,data,bytes,ticket);
}
int pw_vk_replay_enqueue_group(struct pw_vk_replay *s,struct pw_vk_replay_lane *const *lanes,
                              unsigned count,unsigned ordered,const void *data,size_t bytes)
{
 return enqueue_group(s,lanes,count,ordered,data,bytes,NULL);
}

uint64_t pw_vk_replay_marker(struct pw_vk_replay_lane *lane)
{
 uint64_t marker;struct pw_vk_replay *s=lane->owner;
 pthread_mutex_lock(&s->mutex);marker=lane->issued;pthread_mutex_unlock(&s->mutex);return marker;
}
int pw_vk_replay_wait(struct pw_vk_replay_lane *lane,uint64_t ticket)
{
 struct pw_vk_replay *s;int status;uint64_t epoch;
 if(!lane)return PW_VK_REPLAY_INVALID;
 s=lane->owner;
 pthread_mutex_lock(&s->mutex);
 if(ticket>lane->issued){pthread_mutex_unlock(&s->mutex);return PW_VK_REPLAY_INVALID;}
 epoch=s->ordered_issued;
 while(!failed(s) && (lane->done<ticket || s->ordered_done<epoch)){++s->stats.completion_waits;pthread_cond_wait(&s->changed,&s->mutex);}
 status=failed(s)?PW_VK_REPLAY_FAILED:PW_VK_REPLAY_OK;
 pthread_mutex_unlock(&s->mutex);return status;
}
static int wait_scope(struct pw_vk_replay *s,uint64_t pool)
{
 int pending,status;struct pw_vk_replay_lane *lane;uint64_t epoch;
 if(!s)return PW_VK_REPLAY_INVALID;
 pthread_mutex_lock(&s->mutex);epoch=s->ordered_issued;
 do{
  pending=s->ordered_done<epoch || (!pool && s->stats.completed!=s->stats.submitted);
  for(lane=s->lanes;lane;lane=lane->next)
   if((!pool || lane->domain->key==pool) && lane->issued!=lane->done){pending=1;break;}
  if(pending && !failed(s)){++s->stats.completion_waits;pthread_cond_wait(&s->changed,&s->mutex);}
 }while(pending && !failed(s));
 status=failed(s)?PW_VK_REPLAY_FAILED:PW_VK_REPLAY_OK;
 pthread_mutex_unlock(&s->mutex);return status;
}
int pw_vk_replay_wait_pool(struct pw_vk_replay *s,uint64_t pool)
{
 return pool?wait_scope(s,pool):PW_VK_REPLAY_INVALID;
}
int pw_vk_replay_wait_all(struct pw_vk_replay *s){return wait_scope(s,0);}
int pw_vk_replay_lane_drop(struct pw_vk_replay_lane *lane)
{
 struct pw_vk_replay *s;struct pw_vk_replay_lane **link;struct domain **d;int status;
 if(!lane)return PW_VK_REPLAY_INVALID;
 s=lane->owner;
 if((status=pw_vk_replay_wait(lane,pw_vk_replay_marker(lane))))return status;
 pthread_mutex_lock(&s->mutex);
 for(link=&s->lanes;*link!=lane;link=&(*link)->next){}
 *link=lane->next;
 if(!--lane->domain->references){
  for(d=&s->domains;*d!=lane->domain;d=&(*d)->next){}
  *d=lane->domain->next;free(lane->domain);
 }
 free(lane);pthread_mutex_unlock(&s->mutex);return PW_VK_REPLAY_OK;
}
void pw_vk_replay_get_stats(struct pw_vk_replay *s,struct pw_vk_replay_stats *stats)
{
 pthread_mutex_lock(&s->mutex);*stats=s->stats;pthread_mutex_unlock(&s->mutex);
}
void pw_vk_replay_destroy(struct pw_vk_replay *s)
{
 unsigned i;if(!s)return;
 pthread_mutex_lock(&s->mutex);s->stopping=1;pthread_cond_broadcast(&s->changed);pthread_mutex_unlock(&s->mutex);
 for(i=0;i<s->started;i++)pthread_join(s->workers[i].thread,NULL);
 while(s->head){struct job *j=s->head;s->head=j->next;free(j);}
 while(s->lanes){struct pw_vk_replay_lane *l=s->lanes;s->lanes=l->next;free(l);}
 while(s->domains){struct domain *d=s->domains;s->domains=d->next;free(d);}
 pthread_cond_destroy(&s->changed);pthread_mutex_destroy(&s->mutex);free(s);
}
