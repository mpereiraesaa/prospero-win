/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_vk_replay.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
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
struct job {
 struct job *next;
 struct pw_vk_replay_lane *lane;
 uint64_t ticket;
 size_t bytes,charge;
 unsigned char data[];
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
 unsigned started,stopping;
 struct pw_vk_replay_stats stats;
};
static int failed(struct pw_vk_replay *s)
{
 return s->stopping || s->stats.callback_error;
}
static void *worker_main(void *arg)
{
 struct worker *w=arg;struct pw_vk_replay *s=w->owner;
 pthread_mutex_lock(&s->mutex);
 for(;;){
  struct job *job,*previous=NULL;int result;
  if(failed(s))break;
  /* Earliest ready pool wins. Skipping a busy pool allows an unrelated pool
   * to progress; its own first job can never be overtaken. */
  for(job=s->head;job && job->lane->domain->busy;job=job->next)previous=job;
  if(!job){pthread_cond_wait(&s->changed,&s->mutex);continue;}
  if(previous)previous->next=job->next;else s->head=job->next;
  if(s->tail==job)s->tail=previous;
  job->lane->domain->busy=1;
  if(++s->stats.active>s->stats.peak_active)s->stats.peak_active=s->stats.active;
  pthread_mutex_unlock(&s->mutex);
  result=s->execute(job->lane->context,job->data,job->bytes);
  pthread_mutex_lock(&s->mutex);
  if(result && !s->stats.callback_error)s->stats.callback_error=result;
  job->lane->done=job->ticket;job->lane->domain->busy=0;
  --s->stats.active;++s->stats.completed;++s->stats.worker_jobs[w->index];
  s->stats.owned_bytes-=job->charge;free(job);
  pthread_cond_broadcast(&s->changed);
 }
 pthread_mutex_unlock(&s->mutex);return NULL;
}
struct pw_vk_replay *pw_vk_replay_create(unsigned workers,size_t limit,pw_vk_replay_fn execute)
{
 struct pw_vk_replay *s;unsigned i;
 if(!workers || workers>PW_VK_REPLAY_MAX_WORKERS || limit<sizeof(struct job) || !execute)return NULL;
 if(!(s=calloc(1,sizeof(*s))))return NULL;
 s->limit=limit;s->execute=execute;s->stats.workers=workers;
 if(pthread_mutex_init(&s->mutex,NULL)){free(s);return NULL;}
 if(pthread_cond_init(&s->changed,NULL)){pthread_mutex_destroy(&s->mutex);free(s);return NULL;}
 for(i=0;i<workers;i++){
  s->workers[i].owner=s;s->workers[i].index=i;
  if(pthread_create(&s->workers[i].thread,NULL,worker_main,&s->workers[i])){pw_vk_replay_destroy(s);return NULL;}
  ++s->started;
 }
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
int pw_vk_replay_enqueue(struct pw_vk_replay_lane *lane,const void *data,size_t bytes,uint64_t *ticket)
{
 struct pw_vk_replay *s;struct job *job;size_t charge;int status=PW_VK_REPLAY_OK;
 if(!lane || !ticket || (bytes && !data) || bytes>SIZE_MAX-sizeof(*job))return PW_VK_REPLAY_INVALID;
 s=lane->owner;charge=sizeof(*job)+bytes;
 if(charge>s->limit)return PW_VK_REPLAY_INVALID;
 pthread_mutex_lock(&s->mutex);
 while(!failed(s) && charge>s->limit-s->stats.owned_bytes){
  ++s->stats.capacity_waits;pthread_cond_wait(&s->changed,&s->mutex);
 }
 if(failed(s))status=PW_VK_REPLAY_FAILED;
 else if(lane->issued==UINT64_MAX || s->stats.submitted==UINT64_MAX)status=PW_VK_REPLAY_EXHAUSTED;
 else if(!(job=malloc(charge)))status=PW_VK_REPLAY_MEMORY;
 else{
  job->lane=lane;job->ticket=++lane->issued;job->bytes=bytes;job->charge=charge;job->next=NULL;
  if(bytes)memcpy(job->data,data,bytes);
  if(s->tail)s->tail->next=job;else s->head=job;s->tail=job;
  s->stats.owned_bytes+=charge;
  if(s->stats.owned_bytes>s->stats.peak_owned_bytes)s->stats.peak_owned_bytes=s->stats.owned_bytes;
  ++s->stats.submitted;*ticket=job->ticket;pthread_cond_broadcast(&s->changed);
 }
 pthread_mutex_unlock(&s->mutex);return status;
}
uint64_t pw_vk_replay_marker(struct pw_vk_replay_lane *lane)
{
 uint64_t marker;struct pw_vk_replay *s=lane->owner;
 pthread_mutex_lock(&s->mutex);marker=lane->issued;pthread_mutex_unlock(&s->mutex);return marker;
}
int pw_vk_replay_wait(struct pw_vk_replay_lane *lane,uint64_t ticket)
{
 struct pw_vk_replay *s;int status;
 if(!lane)return PW_VK_REPLAY_INVALID;
 s=lane->owner;
 pthread_mutex_lock(&s->mutex);
 if(ticket>lane->issued){pthread_mutex_unlock(&s->mutex);return PW_VK_REPLAY_INVALID;}
 while(!failed(s) && lane->done<ticket){++s->stats.completion_waits;pthread_cond_wait(&s->changed,&s->mutex);}
 status=failed(s)?PW_VK_REPLAY_FAILED:PW_VK_REPLAY_OK;
 pthread_mutex_unlock(&s->mutex);return status;
}
static int wait_scope(struct pw_vk_replay *s,uint64_t pool)
{
 int pending,status;struct pw_vk_replay_lane *lane;
 if(!s)return PW_VK_REPLAY_INVALID;
 pthread_mutex_lock(&s->mutex);
 do{
  pending=0;
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
