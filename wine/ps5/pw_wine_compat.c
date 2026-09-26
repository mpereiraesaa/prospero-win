/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_wine_compat.h"
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

int pw_compat_isatty(int fd)
{
    errno=fcntl(fd,F_GETFD)==-1?EBADF:ENOTTY;
    return 0;
}

int pw_compat_posix_fadvise(int fd,off_t offset,off_t length,int advice)
{
    (void)offset;(void)length;(void)advice;
    return fcntl(fd,F_GETFD)==-1?EBADF:0;  /* returns the error, as posix_fadvise does */
}
unsigned int pw_compat_if_nametoindex(const char *name)
{
    (void)name;errno=ENXIO;
    return 0;
}

int pw_compat_pipe2(int fds[2],int flags)
{
    if(flags&~(O_CLOEXEC|O_NONBLOCK)){errno=EINVAL;return -1;}
    int pair[2];if(pipe(pair))return -1;
    for(int i=0;i<2;i++) {
        int status=0;
        if(flags&O_CLOEXEC)status=fcntl(pair[i],F_SETFD,FD_CLOEXEC);
        if(!status && (flags&O_NONBLOCK)) {
            int current=fcntl(pair[i],F_GETFL);
            status=current<0?-1:fcntl(pair[i],F_SETFL,current|O_NONBLOCK);
        }
        if(status) {
            int saved=errno;close(pair[0]);close(pair[1]);errno=saved;return -1;
        }
    }
    fds[0]=pair[0];fds[1]=pair[1];return 0;
}

static struct timeval to_timeval(const struct timespec *t)
{
    struct timeval v={t->tv_sec,(long)(t->tv_nsec/1000)};return v;
}
int pw_compat_futimens(int fd,const struct timespec times[2])
{
    if(!times)return futimes(fd,NULL);
    if(times[0].tv_nsec==UTIME_OMIT && times[1].tv_nsec==UTIME_OMIT)return 0;
    struct stat st;struct timespec now,chosen[2];
    if(fstat(fd,&st) || clock_gettime(CLOCK_REALTIME,&now))return -1;
    const struct timespec current[2]={st.st_atim,st.st_mtim};
    for(int i=0;i<2;i++) {
        if(times[i].tv_nsec==UTIME_NOW)chosen[i]=now;
        else if(times[i].tv_nsec==UTIME_OMIT)chosen[i]=current[i];
        else if(times[i].tv_nsec<0 || times[i].tv_nsec>=1000000000){errno=EINVAL;return -1;}
        else chosen[i]=times[i];
    }
    struct timeval values[2]={to_timeval(&chosen[0]),to_timeval(&chosen[1])};
    return futimes(fd,values);
}

int pw_compat_posix_fallocate(int fd,off_t offset,off_t length)
{
    (void)fd;(void)offset;(void)length;return EOPNOTSUPP;
}
int pw_compat_no_extattr(void){errno=EOPNOTSUPP;return -1;}

struct passwd *pw_compat_getpwuid(uid_t uid)
{
    static struct passwd record;static char empty[]="";
    const char *home=getenv("HOME"),*user=getenv("USER");
    record=(struct passwd){0};
    record.pw_name=(char *)(user && *user?user:"prospero");
    record.pw_dir=(char *)(home && *home=='/'?home:"/download0/prospero-win");
    record.pw_uid=uid;record.pw_gid=getgid();record.pw_shell=empty;record.pw_passwd=empty;
    return &record;
}
const char *pw_compat_codeset(void){return "UTF-8";}

clock_t pw_compat_times(struct tms *buffer)
{
    long ticks=sysconf(_SC_CLK_TCK);struct rusage usage;struct timespec now;
    if(ticks<=0)ticks=100;
    if(getrusage(RUSAGE_SELF,&usage) || clock_gettime(CLOCK_MONOTONIC,&now))return (clock_t)-1;
    if(buffer) {
        buffer->tms_utime=(clock_t)(usage.ru_utime.tv_sec*ticks+usage.ru_utime.tv_usec*ticks/1000000);
        buffer->tms_stime=(clock_t)(usage.ru_stime.tv_sec*ticks+usage.ru_stime.tv_usec*ticks/1000000);
        buffer->tms_cutime=buffer->tms_cstime=0;
    }
    return (clock_t)(now.tv_sec*ticks+now.tv_nsec/(1000000000/ticks));
}

int pw_compat_uname(char *fields,size_t field_size)
{
    static const char *const values[5]={"FreeBSD","ps5","12.02","prospero-win","amd64"};
    if(!fields || field_size<16){errno=EFAULT;return -1;}
    for(int i=0;i<5;i++) {
        char *field=fields+(size_t)i*field_size;
        memset(field,0,field_size);strncpy(field,values[i],field_size-1);
    }
    return 0;
}

unsigned long pw_compat_cfgetospeed(const struct termios *t){return (unsigned long)t->c_ospeed;}
unsigned long pw_compat_cfgetispeed(const struct termios *t){return (unsigned long)t->c_ispeed;}
int pw_compat_cfsetospeed(struct termios *t,unsigned long speed){t->c_ospeed=(speed_t)speed;return 0;}
int pw_compat_cfsetispeed(struct termios *t,unsigned long speed){t->c_ispeed=(speed_t)speed;return 0;}
