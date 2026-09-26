/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_wine_cwd.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static pthread_rwlock_t cwd_lock = PTHREAD_RWLOCK_INITIALIZER;
static char cwd[PW_CWD_PATH_MAX] = "/";
static char *fd_paths[PW_CWD_MAX_FDS];

static int copy_out(const char *text, size_t length, char *out, size_t size)
{
    if (length >= size) { errno = ENAMETOOLONG; return -1; }
    memcpy(out, text, length);
    out[length] = 0;
    return 0;
}

size_t pw_cwd_get(char *out, size_t size)
{
    size_t length;

    pthread_rwlock_rdlock(&cwd_lock);
    length = strlen(cwd);
    if (copy_out(cwd, length, out, size)) length = 0;
    pthread_rwlock_unlock(&cwd_lock);
    return length;
}

int pw_cwd_fold(const char *absolute, char *out, size_t size)
{
    size_t used = 0;
    const char *p = absolute;

    if (!absolute || absolute[0] != '/' || size < 2) { errno = EINVAL; return -1; }
    while (*p) {
        const char *name;
        size_t length;

        while (*p == '/') p++;
        name = p;
        while (*p && *p != '/') p++;
        length = (size_t)(p - name);
        if (!length || (length == 1 && name[0] == '.')) continue;
        if (length == 2 && name[0] == '.' && name[1] == '.') {
            while (used && out[used] != '/') used--;   /* "/.." stays "/" */
            continue;
        }
        if (used + 1 + length >= size) { errno = ENAMETOOLONG; return -1; }
        out[used++] = '/';
        memcpy(out + used, name, length);
        used += length;
        out[used] = 0;
    }
    if (!used) out[used++] = '/';
    out[used] = 0;
    return 0;
}

int pw_cwd_set(const char *absolute)
{
    char folded[PW_CWD_PATH_MAX];

    if (pw_cwd_fold(absolute, folded, sizeof(folded))) return -1;
    pthread_rwlock_wrlock(&cwd_lock);
    memcpy(cwd, folded, strlen(folded) + 1);
    pthread_rwlock_unlock(&cwd_lock);
    return 0;
}

static int join_locked(const char *base, const char *path, char *out, size_t size)
{
    size_t base_length = strlen(base), path_length = strlen(path);

    if (base_length && base[base_length - 1] == '/') base_length--;
    if (base_length + 1 + path_length >= size) { errno = ENAMETOOLONG; return -1; }
    memcpy(out, base, base_length);
    out[base_length] = '/';
    memcpy(out + base_length + 1, path, path_length + 1);
    return 0;
}

int pw_cwd_join(const char *base, const char *path, char *out, size_t size)
{
    int status;

    if (!path) { errno = EFAULT; return -1; }
    if (path[0] == '/') return copy_out(path, strlen(path), out, size);
    if (!path[0]) { errno = ENOENT; return -1; }
    if (base) return join_locked(base, path, out, size);
    pthread_rwlock_rdlock(&cwd_lock);
    status = join_locked(cwd, path, out, size);
    pthread_rwlock_unlock(&cwd_lock);
    return status;
}

void pw_cwd_track(int fd, const char *absolute)
{
    char *copy;

    if (fd < 0 || fd >= PW_CWD_MAX_FDS || !absolute || absolute[0] != '/') return;
    copy = strdup(absolute);
    pthread_rwlock_wrlock(&cwd_lock);
    free(fd_paths[fd]);
    fd_paths[fd] = copy;           /* NULL when out of memory: untracked */
    pthread_rwlock_unlock(&cwd_lock);
}

void pw_cwd_forget(int fd)
{
    char *old;

    if (fd < 0 || fd >= PW_CWD_MAX_FDS) return;
    pthread_rwlock_wrlock(&cwd_lock);
    old = fd_paths[fd];
    fd_paths[fd] = NULL;
    pthread_rwlock_unlock(&cwd_lock);
    free(old);
}

int pw_cwd_fd_path(int fd, char *out, size_t size)
{
    int status = -1;

    if (fd < 0 || fd >= PW_CWD_MAX_FDS) return -1;
    pthread_rwlock_rdlock(&cwd_lock);
    if (fd_paths[fd]) status = copy_out(fd_paths[fd], strlen(fd_paths[fd]), out, size);
    pthread_rwlock_unlock(&cwd_lock);
    return status;
}

int pw_cwd_resolve_at(int dirfd, int at_fdcwd, const char *path, char *out, size_t size)
{
    int status;

    if (!path) { errno = EFAULT; return -1; }
    if (path[0] == '/' || dirfd == at_fdcwd) return pw_cwd_join(NULL, path, out, size);
    if (!path[0]) { errno = ENOENT; return -1; }
    if (dirfd < 0 || dirfd >= PW_CWD_MAX_FDS) { errno = EBADF; return -1; }
    pthread_rwlock_rdlock(&cwd_lock);
    status = fd_paths[dirfd] ? join_locked(fd_paths[dirfd], path, out, size) : -2;
    pthread_rwlock_unlock(&cwd_lock);
    if (status == -2) { errno = EBADF; status = -1; }
    return status;
}

int pw_cwd_mode_allows(unsigned int st_mode, int want)
{
    return !((want & R_OK) && !(st_mode & (S_IRUSR | S_IRGRP | S_IROTH))) &&
           !((want & W_OK) && !(st_mode & (S_IWUSR | S_IWGRP | S_IWOTH))) &&
           !((want & X_OK) && !(st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)));
}
