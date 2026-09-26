/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* thr_kill2 for wineserver.prx, over the thread registry that ntdll fills;
 * the title libraries have no thr_kill2. */
#include "pw_wine_threads.h"
#include <sys/thr.h>

int thr_kill2(pid_t pid,long id,int sig){return pw_wine_thread_kill(pid,id,sig);}
