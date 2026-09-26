/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_WINE_COMPAT_H
#define PW_WINE_COMPAT_H
#include <stddef.h>
#include <sys/types.h>

/* C library functions Wine's Unix side calls that the PS5 title libraries
 * do not provide, measured by linking ntdll.so, win32u.so and wineserver
 * against the title's stubs. Each has a stated, conservative behaviour;
 * pw_wine_compat_libc.c binds the real names to these on the console. */
struct termios;struct passwd;struct timespec;struct tms;

/* A title cannot create pipes (measured): a socket pair, each end shut
 * in the direction a pipe end cannot be used. */
int pw_compat_pipe(int fds[2]);
int pw_compat_pipe2(int fds[2],int flags);                   /* O_CLOEXEC|O_NONBLOCK only */
int pw_compat_futimens(int fd,const struct timespec times[2]);/* UTIME_NOW/UTIME_OMIT honoured */
int pw_compat_posix_fallocate(int fd,off_t offset,off_t length);/* EOPNOTSUPP */
int pw_compat_no_extattr(void);                               /* -1, errno EOPNOTSUPP */
/* HOME or the title storage root, USER or "prospero"; one static record. */
struct passwd *pw_compat_getpwuid(uid_t uid);
const char *pw_compat_codeset(void);                          /* "UTF-8" */
/* A title has no terminals: 0, errno ENOTTY for an open descriptor, EBADF
 * otherwise. The stubs have isatty only in libScePosixForWebKit, which a
 * game title does not load. */
int pw_compat_isatty(int fd);
/* wineserver's calls, measured by the wineserver.prx link: */
int pw_compat_posix_fadvise(int fd,off_t offset,off_t length,int advice); /* advice only: 0 */
/* No named interfaces are visible to a title: 0, errno ENXIO. */
unsigned int pw_compat_if_nametoindex(const char *name);
/* Exported only by libkernel_sys, which a game title does not get: the
 * firmware leaves these imports at 0 (measured, FW 12.02). */
int pw_compat_no_fstatfs(void);                               /* -1, errno ENOSYS */
int pw_compat_no_link(void);                                  /* -1, errno EPERM */
long pw_compat_no_ptrace(void);                               /* -1, errno EPERM */
mode_t pw_compat_umask(mode_t mask);                          /* remembered, returns the previous */
clock_t pw_compat_times(struct tms *buffer);
/* Five consecutive fields of field_size bytes, as FreeBSD's struct utsname. */
int pw_compat_uname(char *fields,size_t field_size);
unsigned long pw_compat_cfgetospeed(const struct termios *t);
unsigned long pw_compat_cfgetispeed(const struct termios *t);
int pw_compat_cfsetospeed(struct termios *t,unsigned long speed);
int pw_compat_cfsetispeed(struct termios *t,unsigned long speed);

#endif
