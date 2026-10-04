/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Exact added server helpers with native fixture objects and list/refcount
 * callbacks. No Wine process, asynchronous termination, signal or fault test. */
#include "ps5_mutex_word.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct list { struct list *next, *prev; };
#define LIST_INIT(n) { &(n), &(n) }
#define LIST_ENTRY(p,t,m) ((t *)((char *)(p) - offsetof(t,m)))
#define LIST_FOR_EACH_ENTRY(p,h,t,m) \
    for (struct list *it = (h)->next; it != (h) && ((p) = LIST_ENTRY(it,t,m), 1); it = it->next)
static void list_init(struct list *h) { h->next = h->prev = h; }
static int list_empty(const struct list *h) { return h->next == h; }
static struct list *list_head(struct list *h) { return list_empty(h) ? NULL : h->next; }
static void list_add_tail(struct list *h,struct list *p) {
    p->prev=h->prev; p->next=h; h->prev->next=p; h->prev=p;
}
static void list_remove(struct list *p) { p->prev->next=p->next; p->next->prev=p->prev; }
struct object_ops { int kind; };
static const struct object_ops mutex_ops = {1}, mutex_sync_ops = {2};
struct object { unsigned refcount, handle_count; const struct object_ops *ops;
    struct list wait_queue; void *name; };
struct process { struct object obj; struct list thread_list; };
struct thread { struct process *process; unsigned ps5_mutex_token, state;
    struct list proc_entry, mutex_list; };
#define TERMINATED 1
struct ps5_mutex_cell;
struct mutex_sync { struct object obj; struct thread *owner; unsigned count;
    int abandoned; struct list entry; struct ps5_mutex_cell *fast; int fast_disabled; };
struct mutex { struct object obj; struct object *sync; int fast_eligible; };
struct wait_queue_entry { struct list entry; };
typedef unsigned obj_handle_t;
static int inprocess_direct_ok=1;
static struct mutex *handle_object;
static unsigned fixture_access;
static void *grab_object(void *p) { struct object *o=p; assert(o->refcount); ++o->refcount; return p; }
static void release_object(void *p) { struct object *o=p; assert(o->refcount); --o->refcount; }
static struct object *get_handle_obj(struct process *p,obj_handle_t h,unsigned access,
                                     const struct object_ops *ops) {
    (void)p; assert(!access);
    if (h != 4 || !handle_object || handle_object->obj.ops != ops) return NULL;
    return grab_object(handle_object);
}
static unsigned get_handle_access(struct process *p,obj_handle_t h) { (void)p; assert(h==4); return fixture_access; }
static int add_queue(struct object *o,struct wait_queue_entry *e) {
    list_add_tail(&o->wait_queue,&e->entry); return 1;
}
static void remove_queue(struct object *o,struct wait_queue_entry *e) { (void)o; list_remove(&e->entry); }
static void ps5_mutex_enter_slow(struct mutex_sync *m);
/* Fixture legacy ownership operations independently track its list/refcount
 * contract. The shared transition code below is extracted from the patch. */
static void do_grab(struct mutex_sync *m,struct thread *t) {
    ps5_mutex_enter_slow(m); assert(!m->count || m->owner==t);
    if (!m->count++) { assert(!m->owner); grab_object(m); m->owner=t;
        list_add_tail(&t->mutex_list,&m->entry); }
}
static void legacy_release(struct mutex_sync *m,struct thread *t) {
    ps5_mutex_enter_slow(m); assert(m->count && m->owner==t);
    if (!--m->count) { list_remove(&m->entry); m->owner=NULL; release_object(m); }
}
#include "ps5_mutex_server.inc"

static void observe(struct mutex_sync *m,struct thread *t,unsigned expected) {
    uint64_t w=pw_mutex_word_load(&m->fast->word);
    if (w & PW_MUTEX_WORD_SLOW) {
        assert(m->count==expected && m->owner==(expected ? t : NULL));
        assert(m->obj.refcount==2u+!!expected);
        assert(list_empty(&t->mutex_list)==!expected);
    } else {
        assert(pw_mutex_word_count(w)==expected);
        assert(pw_mutex_word_owner(w)==(expected ? t->ps5_mutex_token : 0));
        assert(!m->owner && !m->count && m->obj.refcount==2);
        assert(list_empty(&t->mutex_list));
    }
}
int main(void) {
    struct process p={.obj={.refcount=1}};
    struct thread a={.process=&p},b={.process=&p};
    struct mutex_sync m={.obj={.refcount=1,.ops=&mutex_sync_ops}};
    struct mutex wrapper={.obj={.refcount=1,.handle_count=1,.ops=&mutex_ops},.sync=&m.obj,.fast_eligible=1};
    struct pw_mutex_word *word=NULL; unsigned token=0,access=0,before=0,checked=0;
    list_init(&p.thread_list); list_init(&a.mutex_list); list_init(&b.mutex_list);
    list_add_tail(&p.thread_list,&a.proc_entry); list_add_tail(&p.thread_list,&b.proc_entry);
    list_init(&m.obj.wait_queue); handle_object=&wrapper; fixture_access=0x100000;
    /* Ineligible cold metadata must leave state and output storage alone. */
    wrapper.obj.name=&p; assert(!ps5_get_mutex_word(&a,4,&word,&token,&access));
    assert(!word && !m.fast && p.obj.refcount==1); wrapper.obj.name=NULL;
    wrapper.obj.handle_count=2; assert(!ps5_get_mutex_word(&a,4,&word,&token,&access));
    wrapper.obj.handle_count=1;
    ps5_mutex_server_begin();
    assert(ps5_get_mutex_word(&a,4,&word,&token,&access));
    assert(token && access==fixture_access && p.obj.refcount==2);
    assert((uintptr_t)word%64==0 && (pw_mutex_word_load(word)&PW_MUTEX_WORD_SLOW));
    ps5_mutex_server_end(); observe(&m,&a,0); checked++;
    struct ps5_mutex_cell *cell=m.fast;
    for (unsigned round=0;round<200;round++) {
        unsigned depth=round%7+1;
        for (unsigned i=0;i<depth;i++) {
            assert(pw_mutex_word_try_acquire(word,token)); observe(&m,&a,i+1); checked++;
            ps5_mutex_server_begin(); ps5_mutex_enter_slow(&m); observe(&m,&a,i+1); checked++;
            ps5_mutex_enter_slow(&m); observe(&m,&a,i+1); checked++;
            ps5_mutex_server_end(); observe(&m,&a,i+1); checked++;
        }
        for (unsigned i=depth;i;i--) {
            ps5_mutex_server_begin(); legacy_release(&m,&a); observe(&m,&a,i-1); checked++;
            ps5_mutex_server_end(); observe(&m,&a,i-1); checked++;
        }
    }
    /* Ordinary queued ownership transfer and nested operation publication. */
    struct wait_queue_entry wait;
    assert(pw_mutex_word_try_acquire(word,token));
    ps5_mutex_server_begin(); ps5_mutex_server_begin();
    assert(ps5_mutex_add_queue(&m.obj,&wait)); observe(&m,&a,1); checked++;
    ps5_mutex_server_end(); assert(pw_mutex_word_load(word)&PW_MUTEX_WORD_SLOW);
    legacy_release(&m,&a); do_grab(&m,&b);
    ps5_mutex_remove_queue(&m.obj,&wait);
    assert(pw_mutex_word_load(word)&PW_MUTEX_WORD_SLOW); observe(&m,&b,1); checked++;
    ps5_mutex_server_end(); observe(&m,&b,1); checked++;
    assert(b.ps5_mutex_token && b.ps5_mutex_token!=token);
    assert(pw_mutex_word_try_release(word,b.ps5_mutex_token,&before) && before==1);
    /* Representation boundary returns to fast mode after ordinary release. */
    ps5_mutex_server_begin(); do_grab(&m,&a); m.count=PW_MUTEX_WORD_MAX_COUNT+1u;
    ps5_mutex_server_end(); assert(pw_mutex_word_load(word)&PW_MUTEX_WORD_SLOW);
    ps5_mutex_server_begin(); legacy_release(&m,&a); ps5_mutex_server_end();
    observe(&m,&a,PW_MUTEX_WORD_MAX_COUNT); checked++;
    ps5_mutex_server_begin(); ps5_mutex_enter_slow(&m);
    m.count=1; legacy_release(&m,&a); ps5_mutex_server_end(); observe(&m,&a,0); checked++;
    /* Final owned close leaves the legacy sync pin, and permanently retires
     * the word; the wrapper can vanish independently of the sync. */
    assert(pw_mutex_word_try_acquire(word,token));
    ps5_mutex_server_begin(); assert(ps5_mutex_close_handle(&wrapper.obj,&p,4));
    ps5_mutex_server_end(); assert(!m.fast && m.fast_disabled && !wrapper.fast_eligible);
    assert(pw_mutex_word_load(word)&PW_MUTEX_WORD_SLOW);
    assert(m.owner==&a && m.count==1 && m.obj.refcount==2 && p.obj.refcount==1);
    assert(!ps5_get_mutex_word(&a,4,&word,&token,&access));
    release_object(&m); assert(m.obj.refcount==1); legacy_release(&m,&a); assert(!m.obj.refcount);
    assert(list_empty(&ps5_mutex_active) && list_empty(&ps5_mutex_pending)); checked++;
    /* Fixture teardown only: the production backend intentionally retains
     * the word for the module lifetime. No cached reader exists here. */
    free(cell);
    /* The poll-loop readiness downgrade retires every active cell. */
    struct mutex_sync other={.obj={.refcount=1,.ops=&mutex_sync_ops}};
    struct mutex other_wrapper={.obj={.refcount=1,.handle_count=1,.ops=&mutex_ops},
        .sync=&other.obj,.fast_eligible=1};
    list_init(&other.obj.wait_queue); handle_object=&other_wrapper;
    ps5_mutex_server_begin(); assert(ps5_get_mutex_word(&a,4,&word,&token,&access));
    ps5_mutex_server_end(); struct ps5_mutex_cell *other_cell=other.fast;
    assert(!pw_mutex_word_load(word));
    __atomic_store_n(&inprocess_direct_ok,0,__ATOMIC_RELEASE);
    ps5_mutex_server_disable();
    assert(!other.fast && other.fast_disabled && other.obj.refcount==1 && p.obj.refcount==1);
    assert(pw_mutex_word_load(word)&PW_MUTEX_WORD_SLOW);
    assert(list_empty(&ps5_mutex_active) && list_empty(&ps5_mutex_pending)); free(other_cell); checked++;
    __atomic_store_n(&inprocess_direct_ok,1,__ATOMIC_RELEASE);
    /* Duplication permanently disqualifies an otherwise unactivated object. */
    ps5_mutex_retire_object(&other_wrapper.obj);
    assert(!other_wrapper.fast_eligible && !ps5_get_mutex_word(&a,4,&word,&token,&access)); checked++;
    /* Cold activation can transfer an already owned legacy sync. */
    struct mutex_sync owned={.obj={.refcount=1,.ops=&mutex_sync_ops}};
    struct mutex owned_wrapper={.obj={.refcount=1,.handle_count=1,.ops=&mutex_ops},
        .sync=&owned.obj,.fast_eligible=1};
    list_init(&owned.obj.wait_queue); do_grab(&owned,&b); handle_object=&owned_wrapper;
    ps5_mutex_server_begin(); assert(ps5_get_mutex_word(&a,4,&word,&token,&access));
    ps5_mutex_server_end(); observe(&owned,&b,1); checked++;
    struct ps5_mutex_cell *owned_cell=owned.fast;
    assert(!pw_mutex_word_try_acquire(word,token));
    assert(pw_mutex_word_try_release(word,b.ps5_mutex_token,&before) && before==1);
    ps5_mutex_retire_object(&owned_wrapper.obj);
    assert(!owned.fast && owned.obj.refcount==1 && p.obj.refcount==1);
    free(owned_cell); checked++;
    assert(!ps5_mutex_operation_depth);
    printf("PASS: %u actual server-helper observations, queued/nested recovery, retirement\n",checked);
    return 0;
}
