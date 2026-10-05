/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define FILE_MAPPING_ACCESS 0x100
#define FILE_MAPPING_IMAGE 0x200
#define FILE_DELETE_ON_CLOSE 0x1000
#define SEC_IMAGE 0x1000000
#define STATUS_FILE_CLOSED 0xc0000128u
struct list { struct list *next, *prev; };
static void list_init(struct list *head) { head->next = head->prev = head; }
static int list_empty(const struct list *head) { return head->next == head; }
static void list_add_tail(struct list *head, struct list *item)
{
    item->prev = head->prev; item->next = head;
    head->prev->next = item; head->prev = item;
}
static void list_remove(struct list *item) { item->prev->next = item->next; item->next->prev = item->prev; }
#define LIST_FOR_EACH_ENTRY(pos, head, type, member) \
    for (struct list *iter = (head)->next; iter != (head) && \
         ((pos) = (type *)((char *)iter - offsetof(type, member)), 1); iter = iter->next)
struct object { unsigned int refcount; const void *ops; };
struct async_queue { struct list queue; };
static int async_queued(struct async_queue *q) { return !list_empty(&q->queue); }
struct inode { struct list open, locks, closed; };
struct closed_fd { int unix_fd; unsigned int disp_flags; };
struct fd
{
    struct object obj;
    int unix_fd, poll_index, fs_locks;
    unsigned int access, sharing, options, no_fd_status;
    void *user, *completion;
    struct inode *inode;
    struct closed_fd *closed;
    struct list locks, inode_entry;
    struct async_queue read_q, write_q, wait_q;
};
struct shared_map { struct object obj; };
struct memory_view { struct list entry; struct fd *fd; unsigned int flags; struct shared_map *shared; };
struct process { struct list views; void *debug_obj; };
struct mapping
{
    struct object obj;
    struct fd *fd;
    unsigned int flags;
    struct shared_map *shared;
    void *committed, *exp_name, *ver_res;
};
struct file { int unused; };
static int mapping_ops, config_dir_fd = -1;
static struct { int fd, events; } pollfd[1];
static unsigned int poll_removes, file_requests;
static struct process processes[2];
static void *grab_object(void *ptr) { ((struct object *)ptr)->refcount++; return ptr; }
static void release_object(void *ptr) { assert(((struct object *)ptr)->refcount); ((struct object *)ptr)->refcount--; }
static void remove_poll_user(struct fd *fd, int index)
{
    assert(index == 0 && fd->poll_index == 0);
    poll_removes++;
    pollfd[0].fd = -1; pollfd[0].events = 0;
}
static void enum_processes(int (*cb)(struct process *, void *), void *user)
{
    for (unsigned int i = 0; i < 2; ++i) if (cb(&processes[i], user)) break;
}
static struct file *create_file_for_fd_obj(struct fd *fd, unsigned int access, unsigned int sharing)
{
    static struct file file;
    (void)access; (void)sharing;
    assert(fd->unix_fd != -1); file_requests++; return &file;
}

/* PATCH_BODIES */

static void init_fd(struct fd *fd, struct inode *inode, struct closed_fd *closed, int native_fd)
{
    memset(fd, 0, sizeof(*fd)); memset(inode, 0, sizeof(*inode)); memset(closed, 0, sizeof(*closed));
    list_init(&inode->open); list_init(&inode->locks); list_init(&inode->closed);
    list_init(&fd->locks); list_init(&fd->read_q.queue); list_init(&fd->write_q.queue); list_init(&fd->wait_q.queue);
    list_add_tail(&inode->open, &fd->inode_entry);
    fd->inode = inode; fd->closed = closed; fd->unix_fd = closed->unix_fd = native_fd;
    fd->obj.refcount = 2; fd->access = FILE_MAPPING_ACCESS | FILE_MAPPING_IMAGE;
    fd->sharing = 7; fd->fs_locks = 1; fd->poll_index = 0;
    pollfd[0].fd = -1; pollfd[0].events = 0; poll_removes = file_requests = 0;
    for (unsigned int i = 0; i < 2; ++i) { list_init(&processes[i].views); processes[i].debug_obj = NULL; }
}
static void add_view(struct memory_view *view, struct fd *fd, unsigned int process)
{
    memset(view, 0, sizeof(*view)); view->fd = fd; view->flags = SEC_IMAGE;
    list_add_tail(&processes[process].views, &view->entry);
}
static void reject_guards(int native_fd)
{
    struct fd fd; struct inode inode; struct closed_fd closed; struct list dependency;
    unsigned int guards = 0;
#define REJECT(change) do { init_fd(&fd, &inode, &closed, native_fd); change; \
    assert(!ps5_release_image_view_fd(&fd, 1)); assert(fd.unix_fd == native_fd); \
    assert(fcntl(native_fd, F_GETFD) != -1); assert(!poll_removes); guards++; } while (0)
    REJECT(fd.obj.refcount = 3); /* another mapping or temporary owner */
    REJECT(fd.obj.refcount = 1);
    REJECT(fd.access = FILE_MAPPING_ACCESS);
    REJECT(fd.user = &dependency);
    REJECT(fd.inode = NULL);
    REJECT(fd.closed = NULL);
    REJECT(closed.unix_fd = -1);
    REJECT(closed.disp_flags = 1);
    REJECT(fd.options = FILE_DELETE_ON_CLOSE);
    REJECT(list_add_tail(&fd.locks, &dependency));
    REJECT(list_add_tail(&inode.locks, &dependency));
    REJECT(list_add_tail(&inode.closed, &dependency));
    REJECT(list_add_tail(&fd.read_q.queue, &dependency));
    REJECT(list_add_tail(&fd.write_q.queue, &dependency));
    REJECT(list_add_tail(&fd.wait_q.queue, &dependency));
    REJECT(fd.completion = &dependency);
    REJECT(pollfd[0].fd = native_fd);
    REJECT(pollfd[0].events = 1);
#undef REJECT
    init_fd(&fd, &inode, &closed, native_fd);
    assert(!ps5_release_image_view_fd(&fd, 0));
    assert(!ps5_release_image_view_fd(&fd, 2));
    printf("FD dependency guards: %u plus zero/mismatched view count PASS\n", guards);
}
int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "switch"))
    {
        config_dir_fd = open(argv[2], O_RDONLY | O_DIRECTORY); assert(config_dir_fd != -1);
        int expected = atoi(argv[3]); errno = E2BIG;
        assert(ps5_image_view_fd_release_enabled() == expected && errno == E2BIG);
        assert(setenv("WINE_PS5_IMAGE_VIEW_FD_RELEASE", expected ? "0" : "1", 1) == 0);
        assert(ps5_image_view_fd_release_enabled() == expected);
        close(config_dir_fd); return 0;
    }
    int default_off = argc == 2 && !strcmp(argv[1], "default-off");
    if (!default_off) assert(setenv("WINE_PS5_IMAGE_VIEW_FD_RELEASE", "1", 1) == 0);
    char path[] = "/tmp/pw-image-fd-XXXXXX";
    int native_fd = mkstemp(path); assert(native_fd != -1);
    assert(write(native_fd, "mapped image remains readable", 29) == 29);
    const char *mapped = mmap(NULL, 29, PROT_READ, MAP_PRIVATE, native_fd, 0); assert(mapped != MAP_FAILED);
    if (default_off)
    {
        struct fd fd; struct inode inode; struct closed_fd closed; struct memory_view view;
        init_fd(&fd, &inode, &closed, native_fd); add_view(&view, &fd, 0);
        struct mapping mapping = { .obj = { 1, &mapping_ops }, .fd = &fd, .flags = SEC_IMAGE };
        mapping_destroy(&mapping.obj);
        assert(fd.unix_fd == native_fd && closed.unix_fd == native_fd && fd.obj.refcount == 1);
        assert(!poll_removes && fcntl(native_fd, F_GETFD) != -1);
        close(native_fd); assert(!munmap((void *)mapped, 29)); assert(!unlink(path));
        puts("Default-off mapping destruction retains its backing descriptor PASS");
        return 0;
    }
    reject_guards(native_fd);
    struct fd fd; struct inode inode; struct closed_fd closed; struct memory_view views[2];
    struct shared_map shared = { { 1, NULL } };
    init_fd(&fd, &inode, &closed, native_fd); add_view(&views[0], &fd, 0);
    release_mapping_image_view_fd(&fd, 0, NULL); assert(fd.unix_fd == native_fd); /* data */
    release_mapping_image_view_fd(&fd, SEC_IMAGE, &shared); assert(fd.unix_fd == native_fd);
    views[0].shared = &shared;
    release_mapping_image_view_fd(&fd, SEC_IMAGE, NULL); assert(fd.unix_fd == native_fd);
    views[0].shared = NULL; views[0].flags = 0;
    release_mapping_image_view_fd(&fd, SEC_IMAGE, NULL); assert(fd.unix_fd == native_fd);
    views[0].flags = SEC_IMAGE; processes[0].debug_obj = &shared;
    release_mapping_image_view_fd(&fd, SEC_IMAGE, NULL); assert(fd.unix_fd == native_fd); /* late attach */
    processes[0].debug_obj = NULL;
    add_view(&views[1], &fd, 1); fd.obj.refcount = 4; /* two views, two mapping owners */
    struct mapping mapping = { .obj = { 1, &mapping_ops }, .fd = &fd, .flags = SEC_IMAGE };
    struct mapping other_mapping = mapping;
    mapping_destroy(&mapping.obj); assert(fd.unix_fd == native_fd && fd.obj.refcount == 3);
    mapping_destroy(&other_mapping.obj); assert(fd.unix_fd == -1 && fd.obj.refcount == 2);
    assert(closed.unix_fd == -1 && fd.no_fd_status == STATUS_FILE_CLOSED && !fd.fs_locks);
    assert(poll_removes == 1 && fd.poll_index == -1 && !list_empty(&inode.open));
    assert(!memcmp(mapped, "mapped image remains readable", 29)); /* real host mapping survives close */
    assert(fcntl(native_fd, F_GETFD) == -1 && errno == EBADF);
    assert(!get_view_file(&views[0], 1, 7) && !file_requests); /* no wrong-path debugger reopen */
    assert(!ps5_release_image_view_fd(&fd, 1) && poll_removes == 1);
    struct fd fresh; struct inode fresh_inode; struct closed_fd fresh_closed;
    int reopened = open(path, O_RDONLY); assert(reopened != -1);
    init_fd(&fresh, &fresh_inode, &fresh_closed, reopened);
    list_remove(&fresh.inode_entry);
    list_add_tail(&inode.open, &fresh.inode_entry); fresh.inode = &inode;
    struct fd *found = get_fd_object_for_mapping(&fd, fd.access, fd.sharing);
    assert(found == &fresh); release_object(found); /* released metadata cannot back a new section */
    release_object(&fd); release_object(&fd); assert(fd.obj.refcount == 0);
    if (closed.unix_fd != -1) close(closed.unix_fd); /* retained destructor bookkeeping must not double-close */
    assert(fcntl(reopened, F_GETFD) != -1);
    close(reopened); assert(!munmap((void *)mapped, 29)); assert(!unlink(path));
    puts("Two mapping owners/two processes, metadata reuse, debugger, live mmap and single-close PASS");
    return 0;
}
