/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_WINE_CWD_H
#define PW_WINE_CWD_H
/*
 * A per-module working directory for Wine's Unix side on PS5.
 *
 * A title may not change its working directory: chdir() is EPERM for every
 * path and libc's getcwd() faults (FW 12.02). Wine relies on one anyway:
 * ntdll enters the prefix to create dosdevices and lists directories through
 * fchdir, and wineserver saves the registry from its configuration and
 * server directories. pw_wine_cwd_libc.c wraps those calls (lld --wrap) and
 * resolves relative paths against the directory kept here. Each module links
 * its own copy, so the in-process wineserver has its own directory, as the
 * separate server process has on other hosts.
 *
 * The directory is logical: chdir() folds "." and ".." lexically, like a
 * shell's cd. Paths passed to other calls are only joined to it; the kernel
 * resolves them, so symbolic links keep their meaning.
 */
#include <stddef.h>

enum { PW_CWD_PATH_MAX = 1024, PW_CWD_MAX_FDS = 4096 };

/* The directory relative paths are resolved against; "/" at start. */
size_t pw_cwd_get(char *out, size_t size);
/* Make an absolute directory path current, folded lexically. 0, or -1 with
 * errno ENAMETOOLONG or EINVAL for a relative path. */
int pw_cwd_set(const char *absolute);

/* Join path to base (or to the current directory when base is NULL);
 * absolute paths are copied unchanged. 0, or -1 with errno ENAMETOOLONG. */
int pw_cwd_join(const char *base, const char *path, char *out, size_t size);
/* Fold "//", "." and ".." out of an absolute path. 0, or -1 with errno
 * ENAMETOOLONG or EINVAL for a relative path. */
int pw_cwd_fold(const char *absolute, char *out, size_t size);

/* The path each descriptor was opened by, so fchdir() and the *at() calls can
 * resolve against it; fchdir() checks it still names the descriptor. */
void pw_cwd_track(int fd, const char *absolute);
void pw_cwd_forget(int fd);
/* Copy the recorded path of fd; 0, or -1 when none is recorded. */
int pw_cwd_fd_path(int fd, char *out, size_t size);
/* Resolve path for an *at() call: AT_FDCWD (at_fdcwd) and absolute paths as
 * above, a recorded dirfd by its path. 0, or -1 with errno EBADF when dirfd
 * has no record (the kernel's *at() calls are not linked into a title),
 * ENOENT for an empty path, or ENAMETOOLONG. */
int pw_cwd_resolve_at(int dirfd, int at_fdcwd, const char *path, char *out, size_t size);

/* access() from a file's mode bits, for a title whose access() is EPERM even
 * where open() works: the title is the only user of its own storage, so any
 * of the user, group or other bits grants. want is R_OK/W_OK/X_OK flags. */
int pw_cwd_mode_allows(unsigned int st_mode, int want);
#endif
