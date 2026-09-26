/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The C library names behind pw_wine_compat on PS5. Linked into ntdll.so
 * (exported to win32u.so) and into wineserver; each signature is checked
 * against the SDK's FreeBSD headers. */
#include "pw_wine_compat.h"
#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/extattr.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/thr.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <termios.h>
#include <unistd.h>

int pipe2(int fds[2],int flags){return pw_compat_pipe2(fds,flags);}
int futimens(int fd,const struct timespec times[2]){return pw_compat_futimens(fd,times);}
int posix_fallocate(int fd,off_t offset,off_t length)
{return pw_compat_posix_fallocate(fd,offset,length);}

/* Title storage has no extended attributes. */
ssize_t extattr_get_fd(int fd,int ns,const char *name,void *data,size_t bytes)
{(void)fd;(void)ns;(void)name;(void)data;(void)bytes;return pw_compat_no_extattr();}
ssize_t extattr_get_file(const char *path,int ns,const char *name,void *data,size_t bytes)
{(void)path;(void)ns;(void)name;(void)data;(void)bytes;return pw_compat_no_extattr();}
ssize_t extattr_set_fd(int fd,int ns,const char *name,const void *data,size_t bytes)
{(void)fd;(void)ns;(void)name;(void)data;(void)bytes;return pw_compat_no_extattr();}
int extattr_delete_fd(int fd,int ns,const char *name)
{(void)fd;(void)ns;(void)name;return pw_compat_no_extattr();}

/* No mount table is visible to a title. */
int getmntinfo(struct statfs **mounts,int mode){(void)mode;*mounts=NULL;return 0;}

struct passwd *getpwuid(uid_t uid){return pw_compat_getpwuid(uid);}
int isatty(int fd){return pw_compat_isatty(fd);}
char *nl_langinfo(nl_item item){return (char *)(item==CODESET?pw_compat_codeset():"");}
clock_t times(struct tms *buffer){return pw_compat_times(buffer);}
int __xuname(int field_size,void *fields){return pw_compat_uname(fields,(size_t)field_size);}

speed_t cfgetospeed(const struct termios *t){return (speed_t)pw_compat_cfgetospeed(t);}
speed_t cfgetispeed(const struct termios *t){return (speed_t)pw_compat_cfgetispeed(t);}
int cfsetospeed(struct termios *t,speed_t speed){return pw_compat_cfsetospeed(t,speed);}
int cfsetispeed(struct termios *t,speed_t speed){return pw_compat_cfsetispeed(t,speed);}

/* Thread names are diagnostic only; the title's threads keep theirs. */
int thr_set_name(long id,const char *name){(void)id;(void)name;return 0;}

void __assert(const char *function,const char *file,int line,const char *expression)
{
    fprintf(stderr,"Assertion failed: %s (%s: %s: %d)\n",expression,function?function:"?",file,line);
    abort();
}
