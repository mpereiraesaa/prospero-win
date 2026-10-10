/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* libc functions GnuTLS and nettle (libgnutls.prx) name but the payload
 * SDK's system libraries lack: its assertions abort as assert() would; the
 * console's libc has gmtime but no gmtime_r, so one is made under a lock;
 * a title has no password database (GnuTLS asks for the home directory of
 * its configuration), so there is no entry; and GnuTLS's gnulib names C11's
 * thrd_exit, which the system libraries lack: it is pthread_exit. */
#include <errno.h>
#include <pthread.h>
#include <pwd.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

void __assert(const char *function, const char *file, int line, const char *expression)
{
    (void)function; (void)file; (void)line; (void)expression;
    abort();
}

struct tm *gmtime_r(const time_t *clock, struct tm *result)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    struct tm *shared;

    pthread_mutex_lock(&lock);
    if ((shared = gmtime(clock)))
        *result = *shared;
    pthread_mutex_unlock(&lock);
    return shared ? result : NULL;
}

int getpwuid_r(uid_t uid, struct passwd *entry, char *buffer, size_t size, struct passwd **result)
{
    (void)uid; (void)entry; (void)buffer; (void)size;
    *result = NULL;
    return 0;                   /* no entry, no error */
}

_Noreturn void thrd_exit(int result)
{
    pthread_exit((void *)(intptr_t)result);
}
