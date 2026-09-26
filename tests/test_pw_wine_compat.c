/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include "../wine/ps5/pw_wine_compat.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <termios.h>
#include <unistd.h>

int main(void)
{
    /* pipe2: the flags Wine uses take effect; anything else is refused. */
    int fds[2];
    assert(!pw_compat_pipe2(fds,O_CLOEXEC));
    assert(fcntl(fds[0],F_GETFD)&FD_CLOEXEC && fcntl(fds[1],F_GETFD)&FD_CLOEXEC);
    assert(!(fcntl(fds[0],F_GETFL)&O_NONBLOCK));
    assert(write(fds[1],"x",1)==1);char c;assert(read(fds[0],&c,1)==1 && c=='x');
    close(fds[0]);close(fds[1]);
    assert(!pw_compat_pipe2(fds,O_CLOEXEC|O_NONBLOCK));
    assert(fcntl(fds[0],F_GETFL)&O_NONBLOCK && fcntl(fds[1],F_GETFL)&O_NONBLOCK);
    assert(read(fds[0],&c,1)==-1 && errno==EAGAIN);close(fds[0]);close(fds[1]);
    assert(pw_compat_pipe2(fds,O_APPEND)==-1 && errno==EINVAL);

    /* futimens over futimes. */
    char path[]="/tmp/pw-compat-XXXXXX";int fd=mkstemp(path);assert(fd>=0);unlink(path);
    struct timespec set[2]={{1000000,500000000},{2000000,250000000}};
    assert(!pw_compat_futimens(fd,set));
    struct stat st;assert(!fstat(fd,&st));
    assert(st.st_atim.tv_sec==1000000 && st.st_atim.tv_nsec==500000000);
    assert(st.st_mtim.tv_sec==2000000 && st.st_mtim.tv_nsec==250000000);
    struct timespec keep_atime[2]={{0,UTIME_OMIT},{3000000,0}};
    assert(!pw_compat_futimens(fd,keep_atime) && !fstat(fd,&st));
    assert(st.st_atim.tv_sec==1000000 && st.st_mtim.tv_sec==3000000);
    struct timespec now_mtime[2]={{0,UTIME_OMIT},{0,UTIME_NOW}};
    assert(!pw_compat_futimens(fd,now_mtime) && !fstat(fd,&st));
    assert(st.st_atim.tv_sec==1000000 && st.st_mtim.tv_sec>1700000000);
    struct timespec omit[2]={{0,UTIME_OMIT},{0,UTIME_OMIT}};assert(!pw_compat_futimens(fd,omit));
    struct timespec bad[2]={{0,1000000000},{0,0}};
    assert(pw_compat_futimens(fd,bad)==-1 && errno==EINVAL);
    assert(!pw_compat_futimens(fd,NULL) && !fstat(fd,&st) && st.st_atim.tv_sec>1700000000);
    close(fd);assert(pw_compat_futimens(fd,set)==-1);

    assert(pw_compat_posix_fallocate(0,0,4096)==EOPNOTSUPP);
    errno=0;assert(pw_compat_no_extattr()==-1 && errno==EOPNOTSUPP);

    /* The home directory comes from HOME, else the title storage root. */
    setenv("HOME","/download0/home",1);setenv("USER","player",1);
    struct passwd *pw=pw_compat_getpwuid(1000);
    assert(!strcmp(pw->pw_dir,"/download0/home") && !strcmp(pw->pw_name,"player") && pw->pw_uid==1000);
    setenv("HOME","relative",1);unsetenv("USER");pw=pw_compat_getpwuid(0);
    assert(!strcmp(pw->pw_dir,"/download0/prospero-win") && !strcmp(pw->pw_name,"prospero"));
    assert(!strcmp(pw_compat_codeset(),"UTF-8"));

    struct tms first,second;clock_t a=pw_compat_times(&first);
    for(volatile unsigned long spin=0;spin<50000000ul;spin++){}
    clock_t b=pw_compat_times(&second);
    assert(a!=(clock_t)-1 && b>=a && second.tms_utime>=first.tms_utime && !second.tms_cutime);
    assert(pw_compat_times(NULL)!=(clock_t)-1);

    char fields[5*32];
    assert(!pw_compat_uname(fields,32));
    assert(!strcmp(fields,"FreeBSD") && !strcmp(fields+32,"ps5") && !strcmp(fields+128,"amd64"));
    assert(pw_compat_uname(fields,8)==-1 && errno==EFAULT);

    struct termios t;memset(&t,0,sizeof(t));
    assert(!pw_compat_cfsetospeed(&t,B9600) && !pw_compat_cfsetispeed(&t,B19200));
    assert(pw_compat_cfgetospeed(&t)==B9600 && pw_compat_cfgetispeed(&t)==B19200);
    return 0;
}
