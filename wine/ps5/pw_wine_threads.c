/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_wine_threads.h"
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <unistd.h>

typedef struct Entry { long tid; pthread_t thread; } Entry;

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static Entry entries[PW_WINE_THREADS_MAX];
static unsigned count;

static Entry *find_locked(long tid)
{
    for(unsigned i=0;i<count;i++)
        if(entries[i].tid==tid)return &entries[i];
    return NULL;
}

int pw_wine_thread_register(long tid,pthread_t thread)
{
    int status=0;
    pthread_mutex_lock(&lock);
    Entry *entry=find_locked(tid);
    if(entry)entry->thread=thread;
    else if(count<PW_WINE_THREADS_MAX)entries[count++]=(Entry){tid,thread};
    else status=-1;
    pthread_mutex_unlock(&lock);
    return status;
}
void pw_wine_thread_unregister(long tid)
{
    pthread_mutex_lock(&lock);
    Entry *entry=find_locked(tid);
    if(entry)*entry=entries[--count];
    pthread_mutex_unlock(&lock);
}
/* The call is made with the table locked: a thread leaves the table before
 * it exits, so a handle found here is still a live thread's. */
int pw_wine_thread_kill(pid_t pid,long tid,int signal)
{
    int status;
    if(pid!=getpid()){errno=ESRCH;return -1;}
    pthread_mutex_lock(&lock);
    Entry *entry=find_locked(tid);
    status=entry?pthread_kill(entry->thread,signal):ESRCH;
    pthread_mutex_unlock(&lock);
    if(status){errno=status;return -1;}
    return 0;
}
int pw_wine_thread_set_priority(long tid,int policy,int priority)
{
    struct sched_param param={0};
    int status;
    param.sched_priority=priority;
    pthread_mutex_lock(&lock);
    Entry *entry=find_locked(tid);
    status=entry?pthread_setschedparam(entry->thread,policy,&param):ESRCH;
    pthread_mutex_unlock(&lock);
    return status;
}
