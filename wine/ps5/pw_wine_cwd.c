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

/* Virtual symbolic links: folded absolute link path -> target as given. */
static struct { char *path, *target; } links[PW_CWD_MAX_LINKS];
static size_t link_count;
/* Directories whose table was already looked for, by path hash. */
static char *probed[PW_CWD_MAX_PROBED];

static size_t parent_length(const char *absolute)
{
    const char *slash = strrchr(absolute, '/');
    return slash == absolute ? 1 : (size_t)(slash - absolute);
}

static int find_link_locked(const char *folded)
{
    size_t i;

    for (i = 0; i < link_count; i++) if (!strcmp(links[i].path, folded)) return (int)i;
    return -1;
}

int pw_cwd_link_add(const char *absolute, const char *target)
{
    char folded[PW_CWD_PATH_MAX], *path, *copy;
    int status = 0;

    if (!target) { errno = EFAULT; return -1; }
    if (!target[0] || strpbrk(target, "\t\n")) { errno = ENOENT; return -1; }
    if (pw_cwd_fold(absolute, folded, sizeof(folded))) return -1;
    if (!strcmp(folded, "/") || strpbrk(folded, "\t\n")) { errno = EINVAL; return -1; }
    path = strdup(folded);
    copy = strdup(target);
    pthread_rwlock_wrlock(&cwd_lock);
    if (!path || !copy) { errno = ENOMEM; status = -1; }
    else if (find_link_locked(folded) >= 0) { errno = EEXIST; status = -1; }
    else if (link_count == PW_CWD_MAX_LINKS) { errno = ENOSPC; status = -1; }
    else {
        links[link_count].path = path;
        links[link_count++].target = copy;
        path = copy = NULL;
    }
    pthread_rwlock_unlock(&cwd_lock);
    free(path);
    free(copy);
    return status;
}

int pw_cwd_link_remove(const char *absolute)
{
    char folded[PW_CWD_PATH_MAX];
    char *path = NULL, *target = NULL;
    int i;

    if (pw_cwd_fold(absolute, folded, sizeof(folded))) return -1;
    pthread_rwlock_wrlock(&cwd_lock);
    if ((i = find_link_locked(folded)) >= 0) {
        path = links[i].path;
        target = links[i].target;
        links[i] = links[--link_count];
    }
    pthread_rwlock_unlock(&cwd_lock);
    if (i < 0) { errno = ENOENT; return -1; }
    free(path);
    free(target);
    return 0;
}

ssize_t pw_cwd_link_target(const char *absolute, char *out, size_t size)
{
    char folded[PW_CWD_PATH_MAX];
    ssize_t length = -1;
    int i;

    if (pw_cwd_fold(absolute, folded, sizeof(folded))) return -1;
    pthread_rwlock_rdlock(&cwd_lock);
    if ((i = find_link_locked(folded)) >= 0) {
        length = (ssize_t)strlen(links[i].target);
        if ((size_t)length > size) length = (ssize_t)size;   /* readlink() truncates */
        memcpy(out, links[i].target, (size_t)length);
    }
    pthread_rwlock_unlock(&cwd_lock);
    if (i < 0) errno = EINVAL;
    return length;
}

int pw_cwd_follow(const char *absolute, int follow_last, char *out, size_t size)
{
    /* done: the resolved directories so far; todo: what is left to walk. A
     * link found on the way puts its target in front of the rest, so ".."
     * after a link leaves the target's directory, as with a real one. */
    char done[PW_CWD_PATH_MAX], todo[PW_CWD_PATH_MAX], next[PW_CWD_PATH_MAX];
    size_t used = 0;
    int hops = 0, status = 0;
    const char *p;

    if (!absolute || absolute[0] != '/') { errno = EINVAL; return -1; }
    pthread_rwlock_rdlock(&cwd_lock);
    if (!link_count || copy_out(absolute, strlen(absolute), todo, sizeof(todo))) {
        pthread_rwlock_unlock(&cwd_lock);
        return copy_out(absolute, strlen(absolute), out, size);
    }
    done[0] = 0;
    p = todo;
    while (*p) {
        const char *name;
        size_t length;
        int i;

        while (*p == '/') p++;
        name = p;
        while (*p && *p != '/') p++;
        length = (size_t)(p - name);
        if (!length || (length == 1 && name[0] == '.')) continue;
        if (length == 2 && name[0] == '.' && name[1] == '.') {
            while (used && done[used] != '/') used--;
            done[used] = 0;
            continue;
        }
        if (used + 1 + length >= sizeof(done)) { errno = ENAMETOOLONG; status = -1; break; }
        done[used++] = '/';
        memcpy(done + used, name, length);
        used += length;
        done[used] = 0;
        while (*p == '/') p++;
        if ((!*p && !follow_last) || (i = find_link_locked(done)) < 0) continue;
        if (++hops > PW_CWD_MAX_HOPS) { errno = ELOOP; status = -1; break; }
        {
            const char *target = links[i].target;
            size_t target_length = strlen(target), rest_length = strlen(p);

            if (target_length + 1 + rest_length >= sizeof(next)) {
                errno = ENAMETOOLONG;
                status = -1;
                break;
            }
            memcpy(next, target, target_length);
            next[target_length] = '/';
            memcpy(next + target_length + 1, p, rest_length + 1);
            memcpy(todo, next, target_length + rest_length + 2);
            p = todo;
            if (target[0] == '/') used = 0;
            else while (used && done[used] != '/') used--;   /* the link's directory */
            done[used] = 0;
        }
    }
    pthread_rwlock_unlock(&cwd_lock);
    if (status) return -1;
    /* No link on the way: the path goes to the kernel as it came. */
    if (!hops) return copy_out(absolute, strlen(absolute), out, size);
    if (!used) done[used++] = '/', done[used] = 0;
    return copy_out(done, used, out, size);
}

static size_t hash_path(const char *text)
{
    size_t hash = 2166136261u;

    while (*text) hash = (hash ^ (unsigned char)*text++) * 16777619u;
    return hash;
}

/* The slot holding absolute, or -1. */
static int find_probed_locked(const char *absolute, size_t slot)
{
    size_t tries;

    for (tries = 0; tries < PW_CWD_MAX_PROBED && probed[slot]; tries++) {
        if (!strcmp(probed[slot], absolute)) return (int)slot;
        slot = (slot + 1) % PW_CWD_MAX_PROBED;
    }
    return -1;
}

int pw_cwd_probe_directory(const char *absolute)
{
    size_t slot = hash_path(absolute) % PW_CWD_MAX_PROBED, tries;
    int needed = 1;

    /* Every file a game creates asks about its directory: a known one takes
     * only the shared lock. */
    pthread_rwlock_rdlock(&cwd_lock);
    if (find_probed_locked(absolute, slot) >= 0) needed = 0;
    pthread_rwlock_unlock(&cwd_lock);
    if (!needed) return 0;
    pthread_rwlock_wrlock(&cwd_lock);
    for (tries = 0; tries < PW_CWD_MAX_PROBED; tries++, slot = (slot + 1) % PW_CWD_MAX_PROBED) {
        if (!probed[slot]) { probed[slot] = strdup(absolute); break; }  /* NULL: probe again */
        if (!strcmp(probed[slot], absolute)) { needed = 0; break; }
    }
    pthread_rwlock_unlock(&cwd_lock);
    return needed;
}

ssize_t pw_cwd_links_format(const char *directory, char *out, size_t size)
{
    size_t used = 0, i, length = strlen(directory);
    int status = 0;

    pthread_rwlock_rdlock(&cwd_lock);
    for (i = 0; i < link_count && !status; i++) {
        const char *name = links[i].path + parent_length(links[i].path);
        size_t name_length, target_length = strlen(links[i].target);

        if (parent_length(links[i].path) != length || strncmp(links[i].path, directory, length))
            continue;
        if (*name == '/') name++;
        name_length = strlen(name);
        if (used + name_length + target_length + 2 >= size) { errno = ENAMETOOLONG; status = -1; break; }
        memcpy(out + used, name, name_length);
        out[used + name_length] = '\t';
        memcpy(out + used + name_length + 1, links[i].target, target_length);
        out[used + name_length + 1 + target_length] = '\n';
        used += name_length + target_length + 2;
    }
    pthread_rwlock_unlock(&cwd_lock);
    if (status) return -1;
    out[used] = 0;
    return (ssize_t)used;
}

int pw_cwd_links_parse(const char *directory, const char *text, size_t length)
{
    const char *end = text + length;
    int added = 0;

    while (text < end) {
        const char *line_end = memchr(text, '\n', (size_t)(end - text));
        const char *tab = memchr(text, '\t', (size_t)((line_end ? line_end : end) - text));
        char path[PW_CWD_PATH_MAX], target[PW_CWD_PATH_MAX], name[PW_CWD_PATH_MAX];

        if (!line_end) line_end = end;
        if (tab && tab > text && tab + 1 < line_end &&
            (size_t)(tab - text) < sizeof(name) && (size_t)(line_end - tab - 1) < sizeof(target)) {
            memcpy(name, text, (size_t)(tab - text));
            name[tab - text] = 0;
            memcpy(target, tab + 1, (size_t)(line_end - tab - 1));
            target[line_end - tab - 1] = 0;
            if (!strchr(name, '/') && strcmp(name, ".") && strcmp(name, "..") &&
                !join_locked(directory, name, path, sizeof(path)) &&
                !pw_cwd_link_add(path, target))
                added++;
        }
        text = line_end + 1;
    }
    return added;
}

int pw_cwd_links_end_with(const char *directory, char c)
{
    size_t i, length = strlen(directory);
    int found = 0;

    pthread_rwlock_rdlock(&cwd_lock);
    for (i = 0; i < link_count && !found; i++) {
        const char *path = links[i].path;
        size_t path_length = strlen(path);

        found = parent_length(path) == length && !strncmp(path, directory, length) &&
                path[path_length - 1] == c;
    }
    pthread_rwlock_unlock(&cwd_lock);
    return found;
}

/* The count of name changes: this module's own until another module's is
 * shared with it. */
static unsigned long own_changes;
static unsigned long *changes = &own_changes;

unsigned long *pw_cwd_changes_counter(void)
{
    return __atomic_load_n(&changes, __ATOMIC_ACQUIRE);
}

void pw_cwd_share_changes(unsigned long *counter)
{
    if (!counter) return;
    __atomic_store_n(&changes, counter, __ATOMIC_RELEASE);
    /* whatever the other module kept was read before these calls counted there */
    pw_cwd_changed();
}

unsigned long pw_cwd_changes(void)
{
    return __atomic_load_n(pw_cwd_changes_counter(), __ATOMIC_ACQUIRE);
}

void pw_cwd_changed(void)
{
    __atomic_add_fetch(pw_cwd_changes_counter(), 1, __ATOMIC_ACQ_REL);
}

/* Kept listings: names NUL-terminated, in readdir() order. */
struct listing {
    dev_t dev;
    ino_t ino;
    struct timespec mtime, ctime;
    unsigned long changes, used;
    char *names;
    size_t bytes, count;
    int reparse_names;
};
static pthread_rwlock_t listing_lock = PTHREAD_RWLOCK_INITIALIZER;
static struct listing listings[PW_CWD_MAX_LISTINGS];
static int listings_on;
static unsigned long listing_clock;
static struct pw_cwd_listing_stats stats;

static void count(unsigned long *counter)
{
    __atomic_add_fetch(counter, 1, __ATOMIC_RELAXED);
}

static int same_time(const struct timespec *a, const struct timespec *b)
{
    return a->tv_sec == b->tv_sec && a->tv_nsec == b->tv_nsec;
}

void pw_cwd_listings_enable(int enable)
{
    char *old[PW_CWD_MAX_LISTINGS];
    size_t i;

    pthread_rwlock_wrlock(&listing_lock);
    __atomic_store_n(&listings_on, !!enable, __ATOMIC_RELEASE);
    for (i = 0; i < PW_CWD_MAX_LISTINGS; i++) {
        old[i] = enable ? NULL : listings[i].names;
        if (!enable) memset(&listings[i], 0, sizeof(listings[i]));
    }
    pthread_rwlock_unlock(&listing_lock);
    for (i = 0; i < PW_CWD_MAX_LISTINGS; i++) free(old[i]);
}

int pw_cwd_listing_visit(const struct stat *dir, unsigned long changes_before, pw_cwd_visit visit,
                         void *context, int *result, int *reparse_names)
{
    struct listing *found = NULL;
    size_t i;

    count(&stats.scans);
    if (!__atomic_load_n(&listings_on, __ATOMIC_ACQUIRE)) {
        count(&stats.reads);
        return 0;
    }
    pthread_rwlock_rdlock(&listing_lock);
    for (i = 0; i < PW_CWD_MAX_LISTINGS && !found; i++) {
        struct listing *l = &listings[i];

        if (l->names && l->dev == dir->st_dev && l->ino == dir->st_ino) found = l;
    }
    /* Read before anything that has changed since, and from the same
     * directory as far as its times tell. */
    if (found && (found->changes != changes_before || !same_time(&found->mtime, &dir->st_mtim) ||
                  !same_time(&found->ctime, &dir->st_ctim)))
        found = NULL;
    if (found) {
        const char *name = found->names;

        /* readers share the lock: the stamp is the one field they write */
        __atomic_store_n(&found->used, __atomic_add_fetch(&listing_clock, 1, __ATOMIC_RELAXED),
                         __ATOMIC_RELAXED);
        *result = 0;
        for (i = 0; i < found->count && !*result; i++, name += strlen(name) + 1)
            *result = visit(name, context);
        *reparse_names = found->reparse_names;
        count(&stats.hits);
    }
    pthread_rwlock_unlock(&listing_lock);
    if (!found) count(&stats.reads);
    return found != NULL;
}

void pw_cwd_listing_store(const struct stat *dir, unsigned long changes_before, const char *names,
                          size_t bytes, size_t count_of_names)
{
    struct listing *slot = NULL, *oldest = NULL;
    char *copy, *old = NULL;
    const char *name = names;
    int reparse_names = 0;
    size_t i;

    if (!__atomic_load_n(&listings_on, __ATOMIC_ACQUIRE) || bytes > PW_CWD_LISTING_BYTES) return;
    if (!(copy = malloc(bytes ? bytes : 1))) return;
    memcpy(copy, names, bytes);
    for (i = 0; i < count_of_names; i++, name += strlen(name) + 1)
        reparse_names |= name[0] && name[strlen(name) - 1] == '?';
    pthread_rwlock_wrlock(&listing_lock);
    if (!listings_on) {        /* turned off meanwhile */
        pthread_rwlock_unlock(&listing_lock);
        free(copy);
        return;
    }
    for (i = 0; i < PW_CWD_MAX_LISTINGS && !slot; i++) {
        struct listing *l = &listings[i];

        if (!l->names || (l->dev == dir->st_dev && l->ino == dir->st_ino)) slot = l;
        else if (!oldest || l->used < oldest->used) oldest = l;
    }
    if (!slot) {
        slot = oldest;
        count(&stats.evictions);
    }
    old = slot->names;
    slot->dev = dir->st_dev;
    slot->ino = dir->st_ino;
    slot->mtime = dir->st_mtim;
    slot->ctime = dir->st_ctim;
    slot->changes = changes_before;
    slot->used = __atomic_add_fetch(&listing_clock, 1, __ATOMIC_RELAXED);
    slot->names = copy;
    slot->bytes = bytes;
    slot->count = count_of_names;
    slot->reparse_names = reparse_names;
    pthread_rwlock_unlock(&listing_lock);
    count(&stats.stores);
    free(old);
}

void pw_cwd_listing_stats(struct pw_cwd_listing_stats *out)
{
    out->scans = __atomic_load_n(&stats.scans, __ATOMIC_RELAXED);
    out->hits = __atomic_load_n(&stats.hits, __ATOMIC_RELAXED);
    out->reads = __atomic_load_n(&stats.reads, __ATOMIC_RELAXED);
    out->stores = __atomic_load_n(&stats.stores, __ATOMIC_RELAXED);
    out->evictions = __atomic_load_n(&stats.evictions, __ATOMIC_RELAXED);
}
