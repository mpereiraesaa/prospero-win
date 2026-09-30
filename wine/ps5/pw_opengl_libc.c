/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* PS5 OpenGL SDK libc shims linked only into win32u.prx. */
#include <errno.h>
#include <stdio.h>
#include <syslog.h>
#include <unistd.h>

int mkstemp(char *template)
{
    (void)template;
    errno = ENOSYS;
    return -1;
}

int mkstemps(char *template, int suffix_length)
{
    (void)template;
    (void)suffix_length;
    errno = ENOSYS;
    return -1;
}

void openlog(const char *ident, int option, int facility)
{
    (void)ident;
    (void)option;
    (void)facility;
}

FILE *popen(const char *command, const char *mode)
{
    (void)command;
    (void)mode;
    errno = ENOSYS;
    return NULL;
}

int pclose(FILE *stream)
{
    (void)stream;
    errno = ECHILD;
    return -1;
}

/* Mesa's C++ users observe this zero-initialized emulated-TLS pointer. */
void mesa_glapi_tls_context_init(void) __asm__("_ZTH23_mesa_glapi_tls_Context");
void mesa_glapi_tls_context_init(void)
{
}
