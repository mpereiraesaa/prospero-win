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
 *
 * A title cannot create symbolic links either: symlink() is libkernel_sys
 * only, and a system call from title code kills the process (FW 12.02,
 * PPRBUG-22859). Wine needs two, dosdevices/c: -> ../drive_c and z: -> /, so
 * links are virtual: kept here, followed by every wrapped call, and saved in
 * a PW_CWD_LINK_TABLE file in the link's directory. Each module reads a
 * directory's table once: the first time a lookup below that directory
 * fails, or before the first file is created in it (an open with O_CREAT of
 * a missing name fails no lookup). A table's target may leave the prefix:
 * "game.log<TAB>/dev/null" sends a game's log nowhere instead of creating it.
 */
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

enum {
    PW_CWD_PATH_MAX = 1024, PW_CWD_MAX_FDS = 4096,
    PW_CWD_MAX_LINKS = 64, PW_CWD_MAX_HOPS = 16, PW_CWD_MAX_PROBED = 1024
};
#define PW_CWD_LINK_TABLE ".pw-symlinks"

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

/* Record a link at an absolute path; target is kept as given, and a relative
 * one is resolved from the link's directory. 0, or -1 with errno EEXIST,
 * ENOSPC (table full), ENOENT (empty target), EINVAL or ENAMETOOLONG. */
int pw_cwd_link_add(const char *absolute, const char *target);
/* Forget the link at absolute; 0, or -1 with errno ENOENT. */
int pw_cwd_link_remove(const char *absolute);
/* readlink() of a virtual link: the target, truncated to size and not
 * terminated; its length, or -1 with errno EINVAL when absolute is no link. */
ssize_t pw_cwd_link_target(const char *absolute, char *out, size_t size);
/* Replace every link among the directories of an absolute path, and the path
 * itself when follow_last, by its target. A path through no link is copied
 * unchanged. 0, or -1 with errno ELOOP or ENAMETOOLONG. */
int pw_cwd_follow(const char *absolute, int follow_last, char *out, size_t size);
/* 1 the first time a directory is asked about (its table should be read), 0
 * after that, whether or not it had one. */
int pw_cwd_probe_directory(const char *absolute);
/* The table of the links in directory, one "name<TAB>target" line each;
 * its length, or -1 with errno ENAMETOOLONG. */
ssize_t pw_cwd_links_format(const char *directory, char *out, size_t size);
/* Record the links of a table read from directory; how many were new. */
int pw_cwd_links_parse(const char *directory, const char *text, size_t length);
/* 1 when a link of directory (folded) has a name ending in c, else 0. */
int pw_cwd_links_end_with(const char *directory, char c);

/*
 * Directory listings for names that are not found.
 *
 * Windows names ignore case, so when Wine does not find a file under its
 * exact name it reads the file's whole directory looking for another
 * spelling, and then reads it again for the name with a "?" appended, which
 * is how Wine stores a reparse point. On the PS5 that is about twenty system
 * calls for every lookup that misses (notes/gtaiv/fusionfix-cost.md), and the
 * Ultimate ASI Loader makes one before every file a game opens, in its
 * update folder. pw_cwd_scan_directory() keeps the names in memory instead,
 * keyed by the directory's device and inode, and checks two things before it
 * uses them again:
 *
 * - the count of changes: every wrapped call that can add, remove or rename
 *   a name counts one, after the kernel call returns (an open with O_CREAT
 *   only when it created the file, so a log reopened for every line does not
 *   drop every listing). Each module counts its own calls until
 *   pw_cwd_share_changes() gives it another module's counter: ntdll gives
 *   the in-process server its own, since the server creates, deletes and
 *   renames the files of ntdll's handles;
 * - the directory's modification and change times, which catch writers
 *   outside both modules on file systems that keep them. Whether the PS5's
 *   file systems update a directory's times on every create and delete has
 *   not been measured, so the count is what keeps listings correct.
 *
 * Listings are kept only after pw_cwd_listings_enable(1), which ntdll calls
 * once the server shares its count; until then each scan reads the
 * directory, as Wine did.
 */
enum {
    PW_CWD_MAX_LISTINGS = 32,              /* directories kept */
    PW_CWD_LISTING_BYTES = 64 * 1024,      /* names of a larger one are not */
    PW_CWD_NO_REPARSE_NAMES = 1            /* pw_cwd_scan_directory() flag */
};

/* This module's count of name changes, to share with another module. */
unsigned long *pw_cwd_changes_counter(void);
/* Count this module's changes in *counter (another module's) from now on. */
void pw_cwd_share_changes(unsigned long *counter);
/* The count now, and one more change (both atomic). */
unsigned long pw_cwd_changes(void);
void pw_cwd_changed(void);
/* Keep listings (1) or forget them all and read directories again (0). */
void pw_cwd_listings_enable(int enable);

/* Called with each name of a directory in readdir() order, "." and ".."
 * included, until it returns nonzero. It must not call the wrapped
 * functions: kept listings are visited under a lock. */
typedef int (*pw_cwd_visit)(const char *name, void *context);
/* Visit the names of the directory fstatat(dirfd, path) names, from memory
 * when nothing has changed since they were read. The visitor's nonzero
 * result, 0 after every name, or -1 with errno. After every name, *flags has
 * PW_CWD_NO_REPARSE_NAMES when no name in the directory, virtual links
 * included, ends in "?". */
int pw_cwd_scan_directory(int dirfd, const char *path, pw_cwd_visit visit, void *context,
                          unsigned int *flags);

/* The listing store pw_cwd_scan_directory() uses; dir is the directory's
 * stat and changes the count read before it. 1 when a kept listing was
 * visited (the visitor's result in *result, and in *reparse_names whether
 * a name ends in "?"), 0 when there is none to use. */
int pw_cwd_listing_visit(const struct stat *dir, unsigned long changes, pw_cwd_visit visit,
                         void *context, int *result, int *reparse_names);
/* Keep a copy of names, count NUL-terminated names in bytes bytes, when
 * listings are on and they fit. */
void pw_cwd_listing_store(const struct stat *dir, unsigned long changes, const char *names,
                          size_t bytes, size_t count);

struct pw_cwd_listing_stats {
    unsigned long scans;      /* pw_cwd_scan_directory() calls that got a directory */
    unsigned long hits;       /* answered from a kept listing */
    unsigned long reads;      /* read from the directory */
    unsigned long stores;     /* reads kept */
    unsigned long evictions;  /* kept listings replaced by another directory's */
};
void pw_cwd_listing_stats(struct pw_cwd_listing_stats *out);
#endif
