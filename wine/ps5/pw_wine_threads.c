/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_wine_threads.h"
#include <errno.h>
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
int pw_wine_thread_kill(pid_t pid,long tid,int signal)
{
    pthread_t thread;
    int found=0,status;
    if(pid!=getpid()){errno=ESRCH;return -1;}
    pthread_mutex_lock(&lock);
    Entry *entry=find_locked(tid);
    if(entry){thread=entry->thread;found=1;}
    pthread_mutex_unlock(&lock);
    if(!found){errno=ESRCH;return -1;}
    if((status=pthread_kill(thread,signal))){errno=status;return -1;}
    return 0;
}
