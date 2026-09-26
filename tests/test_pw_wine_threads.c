/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include "../wine/ps5/pw_wine_threads.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

static atomic_int hits;
static _Atomic pthread_t hit_thread;
static atomic_int ready,stop;

static void on_signal(int signal){(void)signal;atomic_fetch_add(&hits,1);atomic_store(&hit_thread,pthread_self());}
static void *worker(void *arg)
{
    (void)arg;atomic_store(&ready,1);
    while(!atomic_load(&stop))usleep(1000);
    return NULL;
}

int main(void)
{
    struct sigaction action;memset(&action,0,sizeof(action));
    action.sa_handler=on_signal;sigemptyset(&action.sa_mask);
    assert(!sigaction(SIGUSR1,&action,NULL));

    pthread_t thread;assert(!pthread_create(&thread,NULL,worker,NULL));
    while(!atomic_load(&ready))usleep(1000);

    /* A registered id reaches its own thread, not the caller. */
    assert(!pw_wine_thread_register(4242,thread));
    assert(!pw_wine_thread_kill(getpid(),4242,SIGUSR1));
    for(int i=0;i<1000 && !atomic_load(&hits);i++)usleep(1000);
    assert(atomic_load(&hits)==1 && pthread_equal(atomic_load(&hit_thread),thread));
    /* Signal 0 checks only. */
    assert(!pw_wine_thread_kill(getpid(),4242,0) && atomic_load(&hits)==1);

    /* Unknown ids and other processes are ESRCH. */
    errno=0;assert(pw_wine_thread_kill(getpid(),7,SIGUSR1)==-1 && errno==ESRCH);
    errno=0;assert(pw_wine_thread_kill(getpid()+1,4242,SIGUSR1)==-1 && errno==ESRCH);

    /* Registering an id again replaces it; unregistering removes it. */
    assert(!pw_wine_thread_register(4242,pthread_self()));
    assert(!pw_wine_thread_register(4243,thread));
    pw_wine_thread_unregister(4242);
    errno=0;assert(pw_wine_thread_kill(getpid(),4242,0)==-1 && errno==ESRCH);
    assert(!pw_wine_thread_kill(getpid(),4243,0));
    pw_wine_thread_unregister(4243);pw_wine_thread_unregister(99);

    /* A full table refuses new ids but still replaces known ones. */
    for(long tid=1;tid<=PW_WINE_THREADS_MAX;tid++)assert(!pw_wine_thread_register(tid,thread));
    assert(pw_wine_thread_register(PW_WINE_THREADS_MAX+1,thread)==-1);
    assert(!pw_wine_thread_register(1,pthread_self()));

    atomic_store(&stop,1);assert(!pthread_join(thread,NULL));
    return 0;
}
