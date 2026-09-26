/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The C library names behind pw_wine_cwd. Each module is linked with
 * --wrap=<name> for every __wrap_<name> below (tools/build_wine_ps5.sh and
 * the Makefile derive the list from this file), so the module's own calls
 * come here and __real_<name> is the kernel's. Relative paths are resolved
 * against the module's directory; absolute paths pass through unchanged.
 * Every successful open records its path, without a system call, so that
 * fchdir() and the *at() calls can resolve against the descriptor.
 *
 * A title is not given libkernel_sys (FW 12.02): its openat(), fstatat(),
 * mkdirat(), unlinkat(), renameat(), symlink(), symlinkat(), readlink(),
 * link() and statfs() imports stay 0, so none of them is called here. The *at
 * calls resolve to an absolute path and use the libkernel call by path; the
 * four with no such call enter the kernel directly. */
#include "pw_wine_cwd.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/vfs.h>
#else
#include <sys/mount.h>
#include <sys/syscall.h>
#endif

#define RESOLVE(path, out) \
    char out[PW_CWD_PATH_MAX]; \
    if (pw_cwd_join(NULL, (path), out, sizeof(out))) return -1

int __real_open(const char *path, int flags, ...);
int __real_close(int fd);
int __real_stat(const char *path, struct stat *st);
int __real_lstat(const char *path, struct stat *st);
int __real_access(const char *path, int mode);
int __real_mkdir(const char *path, mode_t mode);
int __real_rmdir(const char *path);
int __real_unlink(const char *path);
int __real_rename(const char *from, const char *to);
int __real_chmod(const char *path, mode_t mode);
int __real_truncate(const char *path, off_t length);
int __real_utimes(const char *path, const struct timeval times[2]);
char *__real_realpath(const char *path, char *resolved);
DIR *__real_opendir(const char *path);

#ifdef __linux__
/* The host test runs against the C library. */
int __real_symlink(const char *target, const char *path);
ssize_t __real_readlink(const char *path, char *buffer, size_t size);
int __real_link(const char *from, const char *to);
int __real_statfs(const char *path, struct statfs *buffer);
#define kernel_symlink __real_symlink
#define kernel_readlink __real_readlink
#define kernel_link __real_link
#define kernel_statfs __real_statfs
#else
/* FreeBSD 11 numbers, from the SDK's <sys/syscall.h>; the carry flag reports
 * an error, whose number is left in rax. */
static long kernel_call(long number, long a1, long a2, long a3)
{
    unsigned char failed;

    __asm__ __volatile__("syscall"
                         : "+a"(number), "=@ccc"(failed), "+D"(a1), "+S"(a2), "+d"(a3)
                         :
                         : "rcx", "r8", "r9", "r10", "r11", "memory");
    if (failed) { errno = (int)number; return -1; }
    return number;
}

static int kernel_symlink(const char *target, const char *path)
{
    return (int)kernel_call(SYS_symlink, (long)target, (long)path, 0);
}

static ssize_t kernel_readlink(const char *path, char *buffer, size_t size)
{
    return kernel_call(SYS_readlink, (long)path, (long)buffer, (long)size);
}

static int kernel_link(const char *from, const char *to)
{
    return (int)kernel_call(SYS_link, (long)from, (long)to, 0);
}

static int kernel_statfs(const char *path, struct statfs *buffer)
{
    return (int)kernel_call(SYS_statfs, (long)path, (long)buffer, 0);
}
#endif

int __wrap_chdir(const char *path)
{
    char folded[PW_CWD_PATH_MAX];
    struct stat st;
    RESOLVE(path, full);

    if (pw_cwd_fold(full, folded, sizeof(folded)) || __real_stat(folded, &st)) return -1;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    return pw_cwd_set(folded);
}

int __wrap_fchdir(int fd)
{
    char path[PW_CWD_PATH_MAX];
    struct stat by_fd, by_path;

    if (fstat(fd, &by_fd)) return -1;
    if (!S_ISDIR(by_fd.st_mode)) { errno = ENOTDIR; return -1; }
    /* Another module may have closed and reused the number: the path must
     * still name the descriptor's directory. */
    if (pw_cwd_fd_path(fd, path, sizeof(path)) || __real_stat(path, &by_path) ||
        by_path.st_dev != by_fd.st_dev || by_path.st_ino != by_fd.st_ino) {
        errno = EACCES;
        return -1;
    }
    return pw_cwd_set(path);
}

char *__wrap_getcwd(char *buffer, size_t size)
{
    char path[PW_CWD_PATH_MAX];
    size_t length = pw_cwd_get(path, sizeof(path));

    if (!buffer) {
        if (!size) size = length + 1;
        if (!(buffer = malloc(size))) return NULL;
        if (size <= length) { free(buffer); errno = ERANGE; return NULL; }
    }
    else if (!size) { errno = EINVAL; return NULL; }
    if (size <= length) { errno = ERANGE; return NULL; }
    memcpy(buffer, path, length + 1);
    return buffer;
}

int __wrap_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    int fd;
    RESOLVE(path, full);

    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    if ((fd = __real_open(full, flags, mode)) >= 0) pw_cwd_track(fd, full);
    return fd;
}

int __wrap_openat(int dirfd, const char *path, int flags, ...)
{
    char full[PW_CWD_PATH_MAX];
    mode_t mode = 0;
    int fd;

    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    if (pw_cwd_resolve_at(dirfd, AT_FDCWD, path, full, sizeof(full))) return -1;
    if ((fd = __real_open(full, flags, mode)) >= 0) pw_cwd_track(fd, full);
    return fd;
}

int __wrap_close(int fd)
{
    pw_cwd_forget(fd);
    return __real_close(fd);
}

int __wrap_stat(const char *path, struct stat *st) { RESOLVE(path, full); return __real_stat(full, st); }
int __wrap_lstat(const char *path, struct stat *st) { RESOLVE(path, full); return __real_lstat(full, st); }

int __wrap_fstatat(int dirfd, const char *path, struct stat *st, int flags)
{
    char full[PW_CWD_PATH_MAX];

    if (pw_cwd_resolve_at(dirfd, AT_FDCWD, path, full, sizeof(full))) return -1;
    return flags & AT_SYMLINK_NOFOLLOW ? __real_lstat(full, st) : __real_stat(full, st);
}

int __wrap_access(const char *path, int mode)
{
    struct stat st;
    RESOLVE(path, full);

    if (!__real_access(full, mode)) return 0;
    if (errno != EPERM || __real_stat(full, &st)) return -1;
    if (pw_cwd_mode_allows(st.st_mode, mode)) return 0;
    errno = EACCES;
    return -1;
}

int __wrap_mkdir(const char *path, mode_t mode) { RESOLVE(path, full); return __real_mkdir(full, mode); }

int __wrap_mkdirat(int dirfd, const char *path, mode_t mode)
{
    char full[PW_CWD_PATH_MAX];

    if (pw_cwd_resolve_at(dirfd, AT_FDCWD, path, full, sizeof(full))) return -1;
    return __real_mkdir(full, mode);
}

int __wrap_rmdir(const char *path) { RESOLVE(path, full); return __real_rmdir(full); }
int __wrap_unlink(const char *path) { RESOLVE(path, full); return __real_unlink(full); }

int __wrap_unlinkat(int dirfd, const char *path, int flags)
{
    char full[PW_CWD_PATH_MAX];

    if (pw_cwd_resolve_at(dirfd, AT_FDCWD, path, full, sizeof(full))) return -1;
    return flags & AT_REMOVEDIR ? __real_rmdir(full) : __real_unlink(full);
}

int __wrap_rename(const char *from, const char *to)
{
    char full_to[PW_CWD_PATH_MAX];
    RESOLVE(from, full_from);

    if (pw_cwd_join(NULL, to, full_to, sizeof(full_to))) return -1;
    return __real_rename(full_from, full_to);
}

int __wrap_renameat(int from_dirfd, const char *from, int to_dirfd, const char *to)
{
    char full_from[PW_CWD_PATH_MAX], full_to[PW_CWD_PATH_MAX];

    if (pw_cwd_resolve_at(from_dirfd, AT_FDCWD, from, full_from, sizeof(full_from)) ||
        pw_cwd_resolve_at(to_dirfd, AT_FDCWD, to, full_to, sizeof(full_to)))
        return -1;
    return __real_rename(full_from, full_to);
}

/* The target is stored as given: a relative target stays relative to the
 * link, which is what Wine's "../drive_c" means. */
int __wrap_symlink(const char *target, const char *path)
{
    RESOLVE(path, full);
    return kernel_symlink(target, full);
}

int __wrap_symlinkat(const char *target, int dirfd, const char *path)
{
    char full[PW_CWD_PATH_MAX];

    if (pw_cwd_resolve_at(dirfd, AT_FDCWD, path, full, sizeof(full))) return -1;
    return kernel_symlink(target, full);
}

ssize_t __wrap_readlink(const char *path, char *buffer, size_t size)
{
    RESOLVE(path, full);
    return kernel_readlink(full, buffer, size);
}

int __wrap_link(const char *from, const char *to)
{
    char full_to[PW_CWD_PATH_MAX];
    RESOLVE(from, full_from);

    if (pw_cwd_join(NULL, to, full_to, sizeof(full_to))) return -1;
    return kernel_link(full_from, full_to);
}

int __wrap_chmod(const char *path, mode_t mode) { RESOLVE(path, full); return __real_chmod(full, mode); }
int __wrap_truncate(const char *path, off_t length) { RESOLVE(path, full); return __real_truncate(full, length); }

int __wrap_utimes(const char *path, const struct timeval times[2])
{
    RESOLVE(path, full);
    return __real_utimes(full, times);
}

int __wrap_statfs(const char *path, struct statfs *buffer)
{
    RESOLVE(path, full);
    return kernel_statfs(full, buffer);
}

char *__wrap_realpath(const char *path, char *resolved)
{
    char full[PW_CWD_PATH_MAX];

    if (pw_cwd_join(NULL, path, full, sizeof(full))) return NULL;
    return __real_realpath(full, resolved);
}

DIR *__wrap_opendir(const char *path)
{
    char full[PW_CWD_PATH_MAX];

    if (pw_cwd_join(NULL, path, full, sizeof(full))) return NULL;
    return __real_opendir(full);
}
