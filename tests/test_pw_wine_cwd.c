/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Linked with the same --wrap list as the PS5 modules, so the calls below go
 * through pw_wine_cwd_libc.c while the process's own directory never moves. */
#include "../wine/ps5/pw_wine_cwd.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/vfs.h>
#include <unistd.h>

int __real_open(const char *path, int flags, ...);
int __real_close(int fd);

static char root[64];

static void expect_cwd(const char *want)
{
    char buffer[PW_CWD_PATH_MAX];
    assert(getcwd(buffer, sizeof(buffer)) == buffer);
    if (strcmp(buffer, want)) { fprintf(stderr, "cwd %s, want %s\n", buffer, want); assert(0); }
}

static const char *at_root(const char *name)
{
    static char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", root, name);
    return path;
}

static void test_fold(void)
{
    char out[PW_CWD_PATH_MAX], small[8];

    assert(!pw_cwd_fold("/a/./b//c/../d/", out, sizeof(out)) && !strcmp(out, "/a/b/d"));
    assert(!pw_cwd_fold("/..", out, sizeof(out)) && !strcmp(out, "/"));
    assert(!pw_cwd_fold("/a/../..", out, sizeof(out)) && !strcmp(out, "/"));
    assert(!pw_cwd_fold("/", out, sizeof(out)) && !strcmp(out, "/"));
    assert(!pw_cwd_fold("/.hidden/..x", out, sizeof(out)) && !strcmp(out, "/.hidden/..x"));
    errno = 0;
    assert(pw_cwd_fold("relative", out, sizeof(out)) == -1 && errno == EINVAL);
    errno = 0;
    assert(pw_cwd_fold("/abcdefgh", small, sizeof(small)) == -1 && errno == ENAMETOOLONG);
    errno = 0;
    assert(pw_cwd_join("/base", "", out, sizeof(out)) == -1 && errno == ENOENT);
    assert(!pw_cwd_join("/base/", "x", out, sizeof(out)) && !strcmp(out, "/base/x"));
    assert(!pw_cwd_join("/base", "/abs", out, sizeof(out)) && !strcmp(out, "/abs"));
    errno = 0;
    assert(pw_cwd_resolve_at(PW_CWD_MAX_FDS + 5, -100, "x", out, sizeof(out)) == -1 && errno == EBADF);
    errno = 0;
    assert(pw_cwd_resolve_at(-5, -100, "x", out, sizeof(out)) == -1 && errno == EBADF);
    assert(!pw_cwd_resolve_at(-5, -100, "/abs", out, sizeof(out)) && !strcmp(out, "/abs"));
    errno = 0;
    assert(pw_cwd_resolve_at(-5, -100, NULL, out, sizeof(out)) == -1 && errno == EFAULT);

    assert(pw_cwd_mode_allows(S_IFREG | 0400, R_OK) && !pw_cwd_mode_allows(S_IFREG | 0400, W_OK));
    assert(pw_cwd_mode_allows(S_IFDIR | 0005, R_OK | X_OK) && !pw_cwd_mode_allows(S_IFREG | 0644, X_OK));
    assert(pw_cwd_mode_allows(S_IFREG, F_OK) && !pw_cwd_mode_allows(S_IFREG | 0020 | 0100, R_OK));
}

int main(void)
{
    char real_before[PATH_MAX] = "", real_after[PATH_MAX] = "", buffer[PW_CWD_PATH_MAX], *allocated;
    char prefix[PATH_MAX], resolved[PATH_MAX], expected[PATH_MAX];
    struct stat st;
    int fd, dir_fd, untracked;

    test_fold();
    snprintf(root, sizeof(root), "/tmp/pw-cwd-XXXXXX");
    assert(mkdtemp(root));
    assert(readlink("/proc/self/cwd", real_before, sizeof(real_before) - 1) > 0);
    expect_cwd("/");

    /* Wine's prefix set-up, relative to the directory it enters. */
    assert(!chdir(root));
    expect_cwd(root);
    assert(!mkdir("prefix", 0777));
    assert(!chdir("prefix"));
    snprintf(prefix, sizeof(prefix), "%s/prefix", root);
    expect_cwd(prefix);
    assert(!mkdir("dosdevices", 0777) && !mkdir("drive_c", 0777));
    assert(!symlink("../drive_c", "dosdevices/c:"));
    assert(!stat("dosdevices/c:", &st) && S_ISDIR(st.st_mode));
    assert(!lstat("dosdevices/c:", &st) && S_ISLNK(st.st_mode));
    assert(readlink("dosdevices/c:", buffer, sizeof(buffer)) == 10 && !memcmp(buffer, "../drive_c", 10));
    assert(!stat(at_root("prefix/drive_c"), &st) && S_ISDIR(st.st_mode));

    /* Files, access() and rename(). */
    assert((fd = open("file", O_CREAT | O_WRONLY | O_TRUNC, 0644)) >= 0);
    assert(write(fd, "wine", 4) == 4 && !close(fd));
    assert(!access("file", R_OK | W_OK) && access("missing", F_OK) == -1 && errno == ENOENT);
    assert(!rename("file", "file2") && !stat(at_root("prefix/file2"), &st) && st.st_size == 4);
    assert(!truncate("file2", 2) && !stat("file2", &st) && st.st_size == 2);
    assert(!chmod("file2", 0600) && !stat("file2", &st) && (st.st_mode & 0777) == 0600);
    assert(realpath("dosdevices/c:", resolved));
    assert(realpath(at_root("prefix/drive_c"), expected) && !strcmp(resolved, expected));

    /* fchdir back into a directory opened by path, then the *at() calls. */
    assert((dir_fd = open(".", O_RDONLY)) >= 0);
    assert(!chdir("/"));
    expect_cwd("/");
    assert(!fchdir(dir_fd));
    expect_cwd(prefix);
    assert(!chdir("/") && !fchdir(dir_fd));
    assert((fd = openat(dir_fd, "file2", O_RDONLY)) >= 0 && read(fd, buffer, 8) == 2 && !close(fd));
    assert(!fstatat(dir_fd, "file2", &st, 0) && st.st_size == 2);
    assert(!mkdirat(dir_fd, "sub", 0777) && !stat(at_root("prefix/sub"), &st));
    assert((fd = openat(AT_FDCWD, "sub", O_RDONLY)) >= 0);
    assert(!fchdir(fd));
    expect_cwd(at_root("prefix/sub"));
    assert(!chdir(".."));
    expect_cwd(prefix);
    assert(!close(fd) && !unlinkat(dir_fd, "sub", AT_REMOVEDIR));
    assert(!symlinkat("file2", dir_fd, "link"));
    assert(!fstatat(dir_fd, "link", &st, AT_SYMLINK_NOFOLLOW) && S_ISLNK(st.st_mode));
    assert(!fstatat(dir_fd, "link", &st, 0) && S_ISREG(st.st_mode) && st.st_size == 2);
    assert(!unlinkat(dir_fd, "link", 0) && lstat("link", &st) == -1 && errno == ENOENT);
    errno = 0;
    assert(fstatat(dir_fd, "", &st, 0) == -1 && errno == ENOENT);
    assert(!renameat(dir_fd, "file2", AT_FDCWD, "file3"));
    assert((fd = openat(dir_fd, "made", O_CREAT | O_WRONLY, 0640)) >= 0 && !close(fd));
    assert(!stat(at_root("prefix/made"), &st) && (st.st_mode & 0777) == 0640);
    assert(!link("made", "hard") && !stat("hard", &st) && st.st_nlink == 2);
    assert(!utimes("hard", (const struct timeval[2]){ { 1000, 0 }, { 2000, 0 } }));
    assert(!stat(at_root("prefix/made"), &st) && st.st_mtime == 2000);
    {
        struct statfs fs;
        DIR *dir;
        struct dirent *entry;
        int seen = 0;
        assert(!statfs("hard", &fs));
        assert((dir = opendir("dosdevices")));
        while ((entry = readdir(dir))) seen += !strcmp(entry->d_name, "c:");
        assert(seen == 1 && !closedir(dir));
    }
    assert(!unlink("hard") && !unlink("made") && !unlink("file3"));

    /* Directories the module did not open, and stale records. The kernel's
     * *at() calls are not linked into a title: an unrecorded dirfd is EBADF,
     * while an absolute path still works through it. */
    assert((untracked = __real_open(root, O_RDONLY)) >= 0);
    errno = 0;
    assert(openat(untracked, "prefix", O_RDONLY) == -1 && errno == EBADF);
    errno = 0;
    assert(fstatat(untracked, "prefix", &st, 0) == -1 && errno == EBADF);
    errno = 0;
    assert(mkdirat(untracked, "new", 0777) == -1 && errno == EBADF);
    errno = 0;
    assert(unlinkat(untracked, "plain", 0) == -1 && errno == EBADF);
    errno = 0;
    assert(symlinkat("x", untracked, "new") == -1 && errno == EBADF);
    errno = 0;
    assert(renameat(untracked, "prefix", AT_FDCWD, "moved") == -1 && errno == EBADF);
    errno = 0;
    assert(renameat(AT_FDCWD, "missing", untracked, "moved") == -1 && errno == EBADF);
    assert(!fstatat(untracked, at_root("prefix"), &st, 0) && S_ISDIR(st.st_mode));
    assert(!mkdirat(untracked, at_root("abs"), 0777) && !unlinkat(untracked, at_root("abs"), AT_REMOVEDIR));
    errno = 0;
    assert(fchdir(untracked) == -1 && errno == EACCES);
    pw_cwd_track(untracked, prefix);          /* a path that names another directory */
    errno = 0;
    assert(fchdir(untracked) == -1 && errno == EACCES);
    assert(!__real_close(untracked));
    errno = 0;
    assert(fchdir(dir_fd + 1000) == -1 && errno == EBADF);
    assert((fd = open("dosdevices/c:", O_RDONLY)) >= 0);
    assert(!fchdir(fd) && !close(fd));
    expect_cwd(at_root("prefix/dosdevices/c:"));

    /* chdir() refusals and getcwd() buffers. */
    errno = 0;
    assert(chdir("missing") == -1 && errno == ENOENT);
    assert((fd = open(at_root("plain"), O_CREAT | O_WRONLY, 0644)) >= 0 && !close(fd));
    errno = 0;
    assert(chdir(at_root("plain")) == -1 && errno == ENOTDIR);
    errno = 0;
    assert(chdir("") == -1 && errno == ENOENT);
    assert(!chdir(root));
    errno = 0;
    assert(!getcwd(buffer, 2) && errno == ERANGE);
    errno = 0;
    assert(!getcwd(buffer, 0) && errno == EINVAL);
    assert((allocated = getcwd(NULL, 0)) && !strcmp(allocated, root));
    free(allocated);

    /* The process never changed directory. */
    assert(readlink("/proc/self/cwd", real_after, sizeof(real_after) - 1) > 0);
    assert(!strcmp(real_before, real_after));

    assert(!unlink("plain") && !rmdir("prefix/drive_c"));
    assert(!unlink("prefix/dosdevices/c:") && !rmdir("prefix/dosdevices"));
    assert(!close(dir_fd) && !rmdir("prefix") && !rmdir(root));
    printf("wine cwd passed: fold, prefix set-up, symlinks, access, fchdir, *at, stale and "
           "untracked descriptors, getcwd buffers, process directory unchanged\n");
    return 0;
}
