/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The directory listings pw_cwd_scan_directory() keeps for Wine's lookups of
 * names that are not found, and the count of name changes that drops them.
 * Linked with the same --wrap list as the PS5 modules, like
 * test_pw_wine_cwd.c, so the calls below go through pw_wine_cwd_libc.c. */
#include "../wine/ps5/pw_wine_cwd.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

int __real_open(const char *path, int flags, ...);
int __real_close(int fd);
int __real_unlink(const char *path);
int __real_utimes(const char *path, const struct timeval times[2]);

static char root[64];

/* Under the test's directory; two at a time stay valid (rename). */
static const char *at(const char *name)
{
    static __thread char paths[2][PATH_MAX];
    static __thread int next;
    char *path = paths[next++ & 1];

    snprintf(path, PATH_MAX, "%s/%s", root, name);
    return path;
}

static void touch(const char *name)
{
    int fd = open(at(name), O_CREAT | O_WRONLY, 0644);
    assert(fd >= 0 && !close(fd));
}

/* Wine's comparison, reduced to ASCII: any case, the first match wins. */
struct find { const char *want; char found[256]; int calls; };

static int find_name(const char *name, void *context)
{
    struct find *f = context;

    f->calls++;
    if (strcasecmp(name, f->want)) return 0;
    snprintf(f->found, sizeof(f->found), "%s", name);
    return 1;
}

/* Scan dir for want: 1 found (its spelling in found), 0 not, -1 failed. */
static int scan(const char *dir, const char *want, char *found, unsigned int *flags)
{
    struct find f = { want, "", 0 };
    unsigned int ignored;
    int result = pw_cwd_scan_directory(AT_FDCWD, at(dir), find_name, &f, flags ? flags : &ignored);

    if (found) strcpy(found, f.found);
    return result;
}

/* Every name seen, in order; 7 once stop_at names were seen. */
struct order { char text[4096]; int names, stop_at; };

static int record(const char *name, void *context)
{
    struct order *o = context;

    strcat(strcat(o->text, name), "/");
    return ++o->names == o->stop_at ? 7 : 0;
}

static struct pw_cwd_listing_stats before;
static void mark(void) { pw_cwd_listing_stats(&before); }
/* What happened since mark(): reads from the directory and kept answers. */
#define expect(reads, hits) expect_at(__LINE__, reads, hits)
static void expect_at(int line, unsigned long reads, unsigned long hits)
{
    struct pw_cwd_listing_stats now;

    pw_cwd_listing_stats(&now);
    if (now.reads - before.reads != reads || now.hits - before.hits != hits) {
        fprintf(stderr, "line %d: reads %lu hits %lu, want %lu %lu\n", line, now.reads - before.reads,
                now.hits - before.hits, reads, hits);
        assert(0);
    }
    before = now;
}

static void test_counting(void)
{
    unsigned long count = pw_cwd_changes();
    FILE *file;
    int fd;

    assert(!mkdir(at("count"), 0777) && pw_cwd_changes() == ++count);
    /* creating counts; opening what exists, to append or truncate, does not */
    assert((fd = open(at("count/log"), O_CREAT | O_WRONLY | O_APPEND, 0644)) >= 0 && !close(fd));
    assert(pw_cwd_changes() == ++count);
    assert((fd = open(at("count/log"), O_CREAT | O_WRONLY | O_APPEND, 0644)) >= 0 && !close(fd));
    assert((fd = openat(AT_FDCWD, at("count/log"), O_CREAT | O_TRUNC | O_WRONLY, 0644)) >= 0 && !close(fd));
    assert((fd = open(at("count/log"), O_RDONLY)) >= 0 && !close(fd));
    assert(pw_cwd_changes() == count);
    errno = 0;
    assert(open(at("count/log"), O_CREAT | O_EXCL | O_WRONLY, 0644) == -1 && errno == EEXIST);
    errno = 0;
    assert(open(at("count/missing/log"), O_CREAT | O_WRONLY, 0644) == -1 && errno == ENOENT);
    /* other refusals are the kernel's answer to the call as asked */
    errno = 0;
    assert(open(at("count"), O_CREAT | O_WRONLY, 0644) == -1 && errno == EISDIR);
    assert(pw_cwd_changes() == count);
    assert((fd = open(at("count/excl"), O_CREAT | O_EXCL | O_WRONLY, 0644)) >= 0 && !close(fd));
    assert(pw_cwd_changes() == ++count);
    /* the C library's own opens to write may create */
    assert((file = fopen(at("count/reg"), "w")) && !fclose(file) && pw_cwd_changes() == ++count);
    assert((file = fopen(at("count/reg"), "r")) && !fclose(file) && pw_cwd_changes() == count);
    assert(!rename(at("count/reg"), at("count/reg2")) && pw_cwd_changes() == ++count);
    assert(!unlink(at("count/reg2")) && pw_cwd_changes() == ++count);
    /* a virtual link saves its directory's table */
    assert(!symlink("log", at("count/link")) && pw_cwd_changes() > count);
    count = pw_cwd_changes();
    assert(!unlink(at("count/link")) && pw_cwd_changes() > count);
    count = pw_cwd_changes();
    assert(!unlink(at("count/log")) && !unlink(at("count/excl")) && pw_cwd_changes() == count + 2);
    assert(!rmdir(at("count")) && pw_cwd_changes() == count + 3);
}

static void test_listing(void)
{
    char found[256];
    unsigned int flags;
    struct stat st;

    assert(!mkdir(at("game"), 0777) && !mkdir(at("game/update"), 0777));
    touch("game/update/Common.rpf");
    touch("game/socialclub_LOG.txt");

    /* off: every scan reads the directory, and still answers */
    mark();
    assert(scan("game/update", "socialclub_log.txt", NULL, &flags) == 0 && flags == PW_CWD_NO_REPARSE_NAMES);
    assert(scan("game/update", "socialclub_log.txt", NULL, NULL) == 0);
    expect(2, 0);

    pw_cwd_listings_enable(1);
    /* a miss, then the same miss and another name from memory */
    assert(scan("game/update", "socialclub_log.txt", NULL, &flags) == 0 && flags == PW_CWD_NO_REPARSE_NAMES);
    expect(1, 0);
    assert(scan("game/update", "socialclub_log.txt", NULL, &flags) == 0 && flags == PW_CWD_NO_REPARSE_NAMES);
    assert(scan("game/update", "COMMON.RPF", found, NULL) == 1 && !strcmp(found, "Common.rpf"));
    expect(0, 2);
    /* the same directory by another path: kept by device and inode */
    assert(scan("game/update/../update/", "common.rpf", found, NULL) == 1);
    expect(0, 1);

    /* created after the miss: found */
    touch("game/update/SocialClub_Log.txt");
    assert(scan("game/update", "socialclub_log.txt", found, NULL) == 1 && !strcmp(found, "SocialClub_Log.txt"));
    expect(1, 0);
    /* deleted after the hit: gone */
    assert(!unlink(at("game/update/SocialClub_Log.txt")));
    assert(scan("game/update", "socialclub_log.txt", NULL, NULL) == 0);
    expect(1, 0);
    /* renamed, here and from another directory */
    assert(!rename(at("game/update/Common.rpf"), at("game/update/Moved.rpf")));
    assert(scan("game/update", "common.rpf", NULL, NULL) == 0);
    assert(scan("game/update", "moved.rpf", found, NULL) == 1 && !strcmp(found, "Moved.rpf"));
    expect(1, 1);
    assert(!rename(at("game/socialclub_LOG.txt"), at("game/update/socialclub_LOG.txt")));
    assert(scan("game/update", "SOCIALCLUB_log.TXT", found, NULL) == 1 && !strcmp(found, "socialclub_LOG.txt"));
    expect(1, 0);
    /* a directory made and removed */
    assert(!mkdir(at("game/update/Sub"), 0777));
    assert(scan("game/update", "sub", found, NULL) == 1);
    assert(!rmdir(at("game/update/Sub")) && scan("game/update", "sub", NULL, NULL) == 0);
    expect(2, 0);
    /* appending to a file that exists drops nothing */
    assert(scan("game/update", "absent", NULL, NULL) == 0);
    expect(0, 1);
    {
        int fd = open(at("game/update/socialclub_LOG.txt"), O_CREAT | O_WRONLY | O_APPEND, 0644);
        assert(fd >= 0 && write(fd, "line\n", 5) == 5 && !close(fd));
    }
    assert(scan("game/update", "absent", NULL, NULL) == 0);
    expect(0, 1);

    /* Wine's reparse points are files named "name?"; a virtual link counts */
    touch("game/update/point?");
    assert(scan("game/update", "absent", NULL, &flags) == 0 && !flags);
    assert(scan("game/update", "absent", NULL, &flags) == 0 && !flags);
    expect(1, 1);
    assert(!unlink(at("game/update/point?")));
    assert(scan("game/update", "absent", NULL, &flags) == 0 && flags == PW_CWD_NO_REPARSE_NAMES);
    assert(!symlink("Moved.rpf", at("game/update/linked?")));
    assert(scan("game/update", "absent", NULL, &flags) == 0 && !flags);
    assert(!unlink(at("game/update/linked?")));
    assert(scan("game/update", "absent", NULL, &flags) == 0 && flags == PW_CWD_NO_REPARSE_NAMES);
    /* a found name says nothing about the others */
    assert(scan("game/update", "moved.rpf", NULL, &flags) == 1 && !flags);
    mark();

    /* A writer outside the wrappers: caught by the directory's times where
     * the file system keeps them (set apart here, since a create in the same
     * clock tick may leave them equal). */
    {
        struct timeval times[2] = { { 1000000, 0 }, { 1000000, 0 } };
        int fd = __real_open(at("game/update/outside"), O_CREAT | O_WRONLY, 0644);

        assert(fd >= 0 && !__real_close(fd) && !__real_utimes(at("game/update"), times));
        assert(scan("game/update", "OUTSIDE", NULL, NULL) == 1);
        assert(!__real_unlink(at("game/update/outside")));
        times[1].tv_sec++;
        assert(!__real_utimes(at("game/update"), times));
        assert(scan("game/update", "outside", NULL, NULL) == 0);
        expect(2, 0);
    }

    /* Another module's count, shared: its changes drop this module's
     * listings, and this module's changes are counted there. */
    {
        static unsigned long server_count = 1000;

        assert(scan("game/update", "absent", NULL, NULL) == 0);
        expect(0, 1);
        pw_cwd_share_changes(&server_count);
        assert(pw_cwd_changes_counter() == &server_count && server_count == 1001);
        assert(scan("game/update", "absent", NULL, NULL) == 0);
        assert(scan("game/update", "absent", NULL, NULL) == 0);
        expect(1, 1);
        __atomic_add_fetch(&server_count, 1, __ATOMIC_RELEASE);   /* the server created a file */
        assert(scan("game/update", "absent", NULL, NULL) == 0);
        expect(1, 0);
        touch("game/update/late");
        assert(server_count == 1003 && scan("game/update", "LATE", NULL, NULL) == 1);
        assert(!unlink(at("game/update/late")) && server_count == 1004);
    }

    /* The visitor sees readdir()'s order, read or kept, and its result is
     * returned. */
    {
        DIR *dir = opendir(at("game/update"));
        struct dirent *entry;
        struct order want = { "", 0, 0 }, got = { "", 0, 0 };
        unsigned int ignored;

        assert(dir);
        touch("game/update/b");
        touch("game/update/a");
        while ((entry = readdir(dir))) record(entry->d_name, &want);
        assert(!closedir(dir) && want.names >= 4);
        mark();
        assert(pw_cwd_scan_directory(AT_FDCWD, at("game/update"), record, &got, &ignored) == 0);
        assert(got.names == want.names && !strcmp(got.text, want.text));
        memset(&got, 0, sizeof(got));
        assert(pw_cwd_scan_directory(AT_FDCWD, at("game/update"), record, &got, &ignored) == 0);
        assert(got.names == want.names && !strcmp(got.text, want.text));
        expect(1, 1);
        memset(&got, 0, sizeof(got));
        got.stop_at = 2;
        assert(pw_cwd_scan_directory(AT_FDCWD, at("game/update"), record, &got, &ignored) == 7);
        assert(got.names == 2);
        assert(!unlink(at("game/update/a")) && !unlink(at("game/update/b")));
    }

    /* failures are the kernel's */
    errno = 0;
    assert(scan("game/missing", "x", NULL, &flags) == -1 && errno == ENOENT && !flags);
    errno = 0;
    assert(scan("game/update/Moved.rpf", "x", NULL, NULL) == -1 && errno == ENOTDIR);
    {
        int untracked = __real_open(root, O_RDONLY);
        unsigned int ignored;
        struct find f = { "x", "", 0 };

        errno = 0;
        assert(pw_cwd_scan_directory(untracked, "game", find_name, &f, &ignored) == -1 && errno == EBADF);
        assert(!__real_close(untracked));
    }

    /* turned off: forgotten */
    mark();
    pw_cwd_listings_enable(0);
    assert(scan("game/update", "absent", NULL, NULL) == 0 && scan("game/update", "absent", NULL, NULL) == 0);
    expect(2, 0);
    pw_cwd_listings_enable(1);
    assert(!stat(at("game/update"), &st));
    assert(!unlink(at("game/update/Moved.rpf")) && !unlink(at("game/update/socialclub_LOG.txt")));
    assert(!rmdir(at("game/update")) && !rmdir(at("game")));
}

/* Links on the way are followed, as for any wrapped call. */
static void test_through_links(void)
{
    char found[256];

    assert(!mkdir(at("prefix"), 0777) && !mkdir(at("prefix/dosdevices"), 0777));
    assert(!mkdir(at("prefix/drive_c"), 0777) && !mkdir(at("prefix/drive_c/Games"), 0777));
    assert(!symlink("../drive_c", at("prefix/dosdevices/c:")));
    touch("prefix/drive_c/Games/GTAIV.exe");
    mark();
    assert(scan("prefix/dosdevices/c:/Games", "gtaiv.EXE", found, NULL) == 1 && !strcmp(found, "GTAIV.exe"));
    assert(scan("prefix/drive_c/Games", "missing", NULL, NULL) == 0);
    expect(1, 1);
    assert(!unlink(at("prefix/dosdevices/c:/Games/GTAIV.exe")) && !rmdir(at("prefix/drive_c/Games")));
    assert(!unlink(at("prefix/dosdevices/c:")) && !rmdir(at("prefix/drive_c")));
    assert(!rmdir(at("prefix/dosdevices")) && !rmdir(at("prefix")));
}

/* More directories than are kept, and one too large to keep. */
static void test_limits(void)
{
    struct pw_cwd_listing_stats a, b;
    char name[64], found[256];
    int i;

    assert(!mkdir(at("many"), 0777));
    for (i = 0; i < PW_CWD_MAX_LISTINGS + 4; i++) {
        snprintf(name, sizeof(name), "many/d%d", i);
        assert(!mkdir(at(name), 0777));
    }
    pw_cwd_listing_stats(&a);
    for (i = 0; i < PW_CWD_MAX_LISTINGS + 4; i++) {
        snprintf(name, sizeof(name), "many/d%d", i);
        assert(scan(name, "x", NULL, NULL) == 0);
    }
    pw_cwd_listing_stats(&b);
    assert(b.stores - a.stores == PW_CWD_MAX_LISTINGS + 4 && b.evictions - a.evictions >= 4);
    /* the most recent are still kept */
    mark();
    snprintf(name, sizeof(name), "many/d%d", PW_CWD_MAX_LISTINGS + 3);
    assert(scan(name, "x", NULL, NULL) == 0);
    expect(0, 1);
    for (i = 0; i < PW_CWD_MAX_LISTINGS + 4; i++) {
        snprintf(name, sizeof(name), "many/d%d", i);
        assert(!rmdir(at(name)));
    }

    /* names past PW_CWD_LISTING_BYTES are read every time, and found */
    assert(!mkdir(at("many/big"), 0777));
    for (i = 0; i < PW_CWD_LISTING_BYTES / 32; i++) {
        snprintf(name, sizeof(name), "many/big/%050d", i);
        touch(name);
    }
    pw_cwd_listing_stats(&a);
    mark();
    assert(scan("many/big", "absent", NULL, NULL) == 0 && scan("many/big", "absent", NULL, NULL) == 0);
    expect(2, 0);
    pw_cwd_listing_stats(&b);
    assert(b.stores == a.stores);
    snprintf(name, sizeof(name), "%050d", 7);
    assert(scan("many/big", name, found, NULL) == 1 && !strcmp(found, name));
    for (i = 0; i < PW_CWD_LISTING_BYTES / 32; i++) {
        snprintf(name, sizeof(name), "many/big/%050d", i);
        assert(!unlink(at(name)));
    }
    assert(!rmdir(at("many/big")) && !rmdir(at("many")));
}

/* Readers in several threads while a writer creates, then deletes: a name
 * created before a reader looks is found, and one deleted before is not.
 * "deleting" is set before the first delete, so a reader that still sees it
 * clear after its scan scanned before any delete. */
enum { FILES = 300, READERS = 4 };
static int created, deleting, deleted;

static void *reader(void *arg)
{
    unsigned int flags, seed = (unsigned int)(size_t)arg;
    char want[32];
    int done = 0;

    while (!done) {
        int c = __atomic_load_n(&created, __ATOMIC_SEQ_CST), d = __atomic_load_n(&deleted, __ATOMIC_SEQ_CST);

        done = d == FILES;
        if (c && !d) {
            int newest, older;

            snprintf(want, sizeof(want), "FILE%d", c - 1);
            newest = scan("conc", want, NULL, NULL);
            snprintf(want, sizeof(want), "file%d", rand_r(&seed) % c);
            older = scan("conc", want, NULL, NULL);
            if (!__atomic_load_n(&deleting, __ATOMIC_SEQ_CST)) assert(newest == 1 && older == 1);
        }
        if (d) {
            snprintf(want, sizeof(want), "File%d", d - 1);
            assert(scan("conc", want, NULL, &flags) == 0 && flags == PW_CWD_NO_REPARSE_NAMES);
        }
        assert(scan("conc2", "nothing", NULL, NULL) == 0);
    }
    return NULL;
}

static void test_threads(void)
{
    pthread_t threads[READERS];
    char name[32];
    int i;

    assert(!mkdir(at("conc"), 0777) && !mkdir(at("conc2"), 0777));
    for (i = 0; i < READERS; i++) assert(!pthread_create(&threads[i], NULL, reader, (void *)(size_t)(i + 1)));
    for (i = 0; i < FILES; i++) {
        snprintf(name, sizeof(name), "conc/file%d", i);
        touch(name);
        __atomic_store_n(&created, i + 1, __ATOMIC_SEQ_CST);
    }
    __atomic_store_n(&deleting, 1, __ATOMIC_SEQ_CST);
    for (i = 0; i < FILES; i++) {
        snprintf(name, sizeof(name), "conc/file%d", i);
        assert(!unlink(at(name)));
        __atomic_store_n(&deleted, i + 1, __ATOMIC_SEQ_CST);
    }
    for (i = 0; i < READERS; i++) assert(!pthread_join(threads[i], NULL));
    assert(!rmdir(at("conc")) && !rmdir(at("conc2")));
}

int main(void)
{
    snprintf(root, sizeof(root), "/tmp/pw-listing-XXXXXX");
    assert(mkdtemp(root));
    test_counting();
    test_listing();
    test_through_links();
    test_limits();
    test_threads();
    assert(!rmdir(root));
    printf("wine cwd listings passed: change count (creates only, renames, deletes, links, fopen), "
           "kept listings for misses and other spellings, create/delete/rename/mkdir/rmdir after, "
           "appends drop nothing, reparse names and links, outside writers by times, shared count, "
           "visitor results and order, failures, off, links on the way, eviction, size cap, threads\n");
    return 0;
}
