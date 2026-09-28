/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* libc functions the PS5 Vulkan driver (libvulkan.prx) names but the
 * payload SDK's libc lacks, or has only in a system module a title does not
 * load. Its shader compiler's diagnostics can start a process, which a title
 * cannot; it writes no temporary files here; its assertions abort as assert()
 * would. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

FILE *popen(const char *command,const char *mode)
{
    (void)command;(void)mode;
    errno=ENOSYS;
    return NULL;
}

int mkstemp(char *pattern)
{
    (void)pattern;
    errno=ENOSYS;
    return -1;
}

int pclose(FILE *stream)
{
    (void)stream;
    errno=ECHILD;
    return -1;
}

void __assert(const char *function,const char *file,int line,const char *expression)
{
    (void)function;(void)file;(void)line;(void)expression;
    abort();
}
