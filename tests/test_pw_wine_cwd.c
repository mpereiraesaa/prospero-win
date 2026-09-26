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
int __real_lstat(const char *path, struct stat *st);
ssize_t __real_readlink(const char *path, char *buffer, size_t size);

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

/* The link table alone, on paths no other test uses. */
static void test_links(void)
{
    char out[PW_CWD_PATH_MAX], small[8], name[32];
    int i;

    errno = 0;
    assert(pw_cwd_link_add("/v/a", NULL) == -1 && errno == EFAULT);
    errno = 0;
    assert(pw_cwd_link_add("/v/a", "") == -1 && errno == ENOENT);
    errno = 0;
    assert(pw_cwd_link_add("/v/a", "x\ny") == -1 && errno == ENOENT);
    errno = 0;
    assert(pw_cwd_link_add("/", "x") == -1 && errno == EINVAL);
    errno = 0;
    assert(pw_cwd_link_add("relative", "x") == -1 && errno == EINVAL);
    assert(!pw_cwd_link_add("/v/./a", "../t") && !pw_cwd_link_add("/v/z", "/"));
    errno = 0;
    assert(pw_cwd_link_add("/v/a", "other") == -1 && errno == EEXIST);

    /* Relative targets from the link's directory, absolute ones as given. */
    assert(!pw_cwd_follow("/v/a/x/y", 1, out, sizeof(out)) && !strcmp(out, "/t/x/y"));
    assert(!pw_cwd_follow("/v/a", 1, out, sizeof(out)) && !strcmp(out, "/t"));
    assert(!pw_cwd_follow("/v/a", 0, out, sizeof(out)) && !strcmp(out, "/v/a"));
    assert(!pw_cwd_follow("/v/z/usr", 1, out, sizeof(out)) && !strcmp(out, "/usr"));
    assert(!pw_cwd_follow("/v/ab//x", 1, out, sizeof(out)) && !strcmp(out, "/v/ab//x"));
    assert(!pw_cwd_follow("/v//./a/../x", 1, out, sizeof(out)) && !strcmp(out, "/x"));
    assert(!pw_cwd_follow("/v/z", 1, out, sizeof(out)) && !strcmp(out, "/"));
    assert(!pw_cwd_follow("/v/a/..", 1, out, sizeof(out)) && !strcmp(out, "/"));
    assert(!pw_cwd_follow("/v/z/..", 1, out, sizeof(out)) && !strcmp(out, "/"));
    errno = 0;
    assert(pw_cwd_follow("v/a", 1, out, sizeof(out)) == -1 && errno == EINVAL);
    errno = 0;
    assert(pw_cwd_follow("/v/a/xxxxxxxx", 1, small, sizeof(small)) == -1 && errno == ENAMETOOLONG);
    assert(pw_cwd_link_target("/v/a", out, sizeof(out)) == 4 && !memcmp(out, "../t", 4));
    assert(pw_cwd_link_target("/v/a", out, 2) == 2 && !memcmp(out, "..", 2));
    errno = 0;
    assert(pw_cwd_link_target("/v/none", out, sizeof(out)) == -1 && errno == EINVAL);
    errno = 0;
    assert(pw_cwd_link_target("rel", out, sizeof(out)) == -1 && errno == EINVAL);

    /* Loops and over-long expansions. */
    assert(!pw_cwd_link_add("/loop/a", "b") && !pw_cwd_link_add("/loop/b", "a"));
    errno = 0;
    assert(pw_cwd_follow("/loop/a/x", 1, out, sizeof(out)) == -1 && errno == ELOOP);
    {
        char deep[PW_CWD_PATH_MAX];
        memset(deep, 'd', sizeof(deep) - 1);
        deep[0] = '/';
        memcpy(deep + 1, "long/", 5);
        deep[sizeof(deep) - 1] = 0;
        assert(!pw_cwd_link_add("/long", "/aaaaaaaaaaaaaaaaaaaa"));
        errno = 0;
        assert(pw_cwd_follow(deep, 1, out, sizeof(out)) == -1 && errno == ENAMETOOLONG);
        assert(!pw_cwd_link_remove("/long"));
    }

    /* The saved table of one directory, and reading one back. */
    {
        ssize_t length = pw_cwd_links_format("/v", out, sizeof(out));
        assert(length == 11 && (!strcmp(out, "a\t../t\nz\t/\n") || !strcmp(out, "z\t/\na\t../t\n")));
        assert(pw_cwd_links_format("/nothing", out, sizeof(out)) == 0 && !out[0]);
        errno = 0;
        assert(pw_cwd_links_format("/v", small, 6) == -1 && errno == ENAMETOOLONG);
        assert(pw_cwd_links_format("/", out, sizeof(out)) == 0);
    }
    {
        static const char table[] = "c:\t../drive_c\nnotab\n\tempty\nsl/ash\tx\n..\tx\n.\tx\n"
                                    "blank\t\nd:\t/\nc:\tagain\ne:\tlast";
        assert(pw_cwd_links_parse("/p/dosdevices", table, sizeof(table) - 1) == 3);
        assert(!pw_cwd_follow("/p/dosdevices/c:/windows", 1, out, sizeof(out)) &&
               !strcmp(out, "/p/drive_c/windows"));
        assert(pw_cwd_link_target("/p/dosdevices/e:", out, sizeof(out)) == 4);
        assert(!pw_cwd_link_remove("/p/dosdevices/c:") && !pw_cwd_link_remove("/p/dosdevices/d:"));
        assert(!pw_cwd_link_remove("/p/dosdevices/e:"));
    }
    errno = 0;
    assert(pw_cwd_link_remove("/p/dosdevices/c:") == -1 && errno == ENOENT);
    errno = 0;
    assert(pw_cwd_link_remove("rel") == -1 && errno == EINVAL);

    /* A full table. */
    for (i = 0; ; i++) {
        snprintf(name, sizeof(name), "/full/%d", i);
        if (pw_cwd_link_add(name, "x")) break;
    }
    assert(errno == ENOSPC && i == PW_CWD_MAX_LINKS - 4);
    while (i--) {
        snprintf(name, sizeof(name), "/full/%d", i);
        assert(!pw_cwd_link_remove(name));
    }
    assert(!pw_cwd_link_remove("/v/a") && !pw_cwd_link_remove("/v/z"));
    assert(!pw_cwd_link_remove("/loop/a") && !pw_cwd_link_remove("/loop/b"));

    assert(pw_cwd_probe_directory("/probe") == 1 && pw_cwd_probe_directory("/probe") == 0);
}

int main(void)
{
    char real_before[PATH_MAX] = "", real_after[PATH_MAX] = "", buffer[PW_CWD_PATH_MAX], *allocated;
    char prefix[PATH_MAX], resolved[PATH_MAX], expected[PATH_MAX];
    struct stat st;
    int fd, dir_fd, untracked;

    test_fold();
    test_links();
    snprintf(root, sizeof(root), "/tmp/pw-cwd-XXXXXX");
    assert(mkdtemp(root));
    assert(__real_readlink("/proc/self/cwd", real_before, sizeof(real_before) - 1) > 0);
    expect_cwd("/");

    /* Wine's prefix set-up, relative to the directory it enters. */
    assert(!chdir(root));
    expect_cwd(root);
    assert(!mkdir("prefix", 0777));
    assert(!chdir("prefix"));
    snprintf(prefix, sizeof(prefix), "%s/prefix", root);
    expect_cwd(prefix);
    assert(!mkdir("dosdevices", 0777) && !mkdir("drive_c", 0777));
    assert(!symlink("../drive_c", "dosdevices/c:") && !symlink("/", "dosdevices/z:"));
    errno = 0;
    assert(symlink("elsewhere", "dosdevices/c:") == -1 && errno == EEXIST);
    errno = 0;
    assert(symlink("x", "drive_c") == -1 && errno == EEXIST);
    errno = 0;
    assert(symlink("x", "missing/link") == -1 && errno == ENOENT);
    {
        char name[32];
        int i;
        for (i = 0; ; i++) {
            snprintf(name, sizeof(name), "/filler/%d", i);
            if (pw_cwd_link_add(name, "x")) break;
        }
        errno = 0;
        assert(symlink("x", "dosdevices/y:") == -1 && errno == ENOSPC);
        while (i--) {
            snprintf(name, sizeof(name), "/filler/%d", i);
            assert(!pw_cwd_link_remove(name));
        }
    }
    /* The links are virtual: nothing but their table is on disk. */
    assert(__real_lstat(at_root("prefix/dosdevices/c:"), &st) == -1 && errno == ENOENT);
    assert(!__real_lstat(at_root("prefix/dosdevices/" PW_CWD_LINK_TABLE), &st) && S_ISREG(st.st_mode));
    assert(!stat("dosdevices/z:/tmp", &st) && S_ISDIR(st.st_mode));
    assert(!mkdir("dosdevices/c:/windows", 0777) && !stat(at_root("prefix/drive_c/windows"), &st));
    assert(!rmdir("dosdevices/c:/windows"));
    assert(!stat("dosdevices/c:", &st) && S_ISDIR(st.st_mode));
    assert(!lstat("dosdevices/c:", &st) && S_ISLNK(st.st_mode));
    assert(readlink("dosdevices/c:", buffer, sizeof(buffer)) == 10 && !memcmp(buffer, "../drive_c", 10));
    errno = 0;
    assert(readlink("drive_c", buffer, sizeof(buffer)) == -1 && errno == EINVAL);
    errno = 0;
    assert(readlink("missing", buffer, sizeof(buffer)) == -1 && errno == ENOENT);
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
    assert(!symlinkat("file2", dir_fd, "link") && !access("link", R_OK));
    errno = 0;
    assert(access("dosdevices/c:/missing", F_OK) == -1 && errno == ENOENT);
    assert(!fstatat(dir_fd, "link", &st, AT_SYMLINK_NOFOLLOW) && S_ISLNK(st.st_mode));
    assert(!fstatat(dir_fd, "link", &st, 0) && S_ISREG(st.st_mode) && st.st_size == 2);
    assert(!unlinkat(dir_fd, "link", 0) && lstat("link", &st) == -1 && errno == ENOENT);
    errno = 0;
    assert(fstatat(dir_fd, "", &st, 0) == -1 && errno == ENOENT);
    assert(!renameat(dir_fd, "file2", AT_FDCWD, "file3"));
    assert((fd = openat(dir_fd, "made", O_CREAT | O_WRONLY, 0640)) >= 0 && !close(fd));
    assert(!stat(at_root("prefix/made"), &st) && (st.st_mode & 0777) == 0640);
    errno = 0;
    assert(link("made", "hard") == -1 && errno == EPERM);
    assert(!utimes("made", (const struct timeval[2]){ { 1000, 0 }, { 2000, 0 } }));
    assert(!stat(at_root("prefix/made"), &st) && st.st_mtime == 2000);
    {
        struct statfs fs;
        DIR *dir;
        struct dirent *entry;
        int seen = 0;
        errno = 0;
        assert(statfs("made", &fs) == -1 && errno == ENOSYS);
        assert((dir = opendir("dosdevices/c:/..")));
        while ((entry = readdir(dir))) seen += !strcmp(entry->d_name, "drive_c");
        assert(seen == 1 && !closedir(dir));
    }
    /* fopen() as wineserver loads the registry: relative to the prefix, and
     * through a link. */
    {
        FILE *file;
        char line[16];
        assert((file = fopen("system.reg", "w")) && fputs("WINE REGISTRY\n", file) >= 0 && !fclose(file));
        assert((file = fopen(at_root("prefix/system.reg"), "r")) && fgets(line, sizeof(line), file));
        assert(!strcmp(line, "WINE REGISTRY\n") && !fclose(file));
        assert((file = fopen("dosdevices/c:/../system.reg", "r")) && !fclose(file));
        errno = 0;
        assert(!fopen("missing.reg", "r") && errno == ENOENT);
        errno = 0;
        assert(!fopen("", "r") && errno == ENOENT);
        assert(!unlink("system.reg"));
    }
    /* Files reached through a link, and renamed across one. */
    assert((fd = open("dosdevices/c:/sys.ini", O_CREAT | O_WRONLY, 0644)) >= 0 && !close(fd));
    assert(!chmod("dosdevices/c:/sys.ini", 0600) && !truncate("dosdevices/c:/sys.ini", 0));
    assert(!rename("dosdevices/c:/sys.ini", "moved.ini") && !stat("moved.ini", &st));
    assert(!unlink("moved.ini") && !unlink("made") && !unlink("file3"));

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
    expect_cwd(at_root("prefix/drive_c"));
    assert(!chdir(at_root("prefix/dosdevices/c:")));
    expect_cwd(at_root("prefix/dosdevices/c:"));
    /* ".." after a link leaves its target, as with a real link. */
    assert(!stat(".", &st) && S_ISDIR(st.st_mode) && !stat("../drive_c", &st));
    errno = 0;
    assert(stat("../z:", &st) == -1 && errno == ENOENT);

    /* A table another module saved, read when a lookup below it fails. */
    {
        int table;
        assert(!mkdir(at_root("other"), 0777));
        assert((table = __real_open(at_root("other/" PW_CWD_LINK_TABLE), O_CREAT | O_WRONLY, 0644)) >= 0);
        assert(write(table, "d:\t../prefix/drive_c\n", 22) == 22 && !__real_close(table));
        assert(!lstat(at_root("other/d:"), &st) && S_ISLNK(st.st_mode) && st.st_size == 17);
        assert(!unlink(at_root("other/d:")));
        assert(__real_lstat(at_root("other/" PW_CWD_LINK_TABLE), &st) == -1 && errno == ENOENT);
        assert(!rmdir(at_root("other")));
    }

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
    assert(__real_readlink("/proc/self/cwd", real_after, sizeof(real_after) - 1) > 0);
    assert(!strcmp(real_before, real_after));

    assert(!unlink("plain") && !rmdir("prefix/drive_c"));
    assert(!unlinkat(AT_FDCWD, "prefix/dosdevices/c:", 0) && !unlink("prefix/dosdevices/z:"));
    assert(!rmdir("prefix/dosdevices"));
    assert(!close(dir_fd) && !rmdir("prefix") && !rmdir(root));
    printf("wine cwd passed: fold, link table, prefix set-up, virtual symlinks and their saved "
           "table, access, fchdir, *at, stale and untracked descriptors, getcwd buffers, process "
           "directory unchanged\n");
    return 0;
}
