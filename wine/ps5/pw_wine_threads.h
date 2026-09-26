/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_WINE_THREADS_H
#define PW_WINE_THREADS_H
#include <pthread.h>
#include <sys/types.h>

/* The in-process wineserver signals Wine's threads (suspend, APCs, kill)
 * with thr_kill2(pid, lwpid, signal), which the title libraries do not
 * provide; they only have pthread_kill. ntdll registers each thread's
 * kernel thread id with its pthread as the thread first talks to the
 * server, and wineserver.prx's thr_kill2 looks the id up. Registering an
 * id again replaces the entry, since the kernel reuses ids. */
enum { PW_WINE_THREADS_MAX = 1024 };

/* 0, or -1 when the table is full. */
int pw_wine_thread_register(long tid,pthread_t thread);
void pw_wine_thread_unregister(long tid);
/* thr_kill2 on the registered threads of this process: 0, or -1 with
 * errno ESRCH for another process or an unknown id. */
int pw_wine_thread_kill(pid_t pid,long tid,int signal);

#endif
