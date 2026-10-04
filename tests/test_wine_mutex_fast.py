#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Bounded native model of the actual patch helpers and public Wine authority.

This executes no Wine runtime, guest program or console operation. Object,
thread, handle-table and module boundaries are explicit native mocks. The
owner must separately run full Wine semantics, startup and performance gates.
"""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "wine/patches/0790-server-ps5-immediate-mutex-calls.patch"


def function(text, name):
    match = re.search(r"^(?:static )?(?:inline )?(?:DECLSPEC_EXPORT )?"
                      r"(?:int|unsigned int) " + name + r"\([^;]*?\)\s*\{", text, re.M)
    assert match, name
    start = text.index("{", match.start())
    end, depth = start + 1, 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[match.start():end]


def added_source():
    parts = {}
    for diff in PATCH.read_text().split("diff --git ")[1:]:
        name = diff.splitlines()[0].split(" b/", 1)[1]
        parts[name] = "\n".join(line[1:] for line in diff.splitlines()
                                 if line.startswith("+") and not line.startswith("+++"))
    return parts


HARNESS = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
typedef unsigned int obj_handle_t;
typedef uint64_t client_ptr_t;
typedef int64_t timeout_t;
typedef void *HANDLE;
typedef int32_t LONG;
typedef struct { int64_t QuadPart; } LARGE_INTEGER;
#define WINE_INPROCESS_SERVER 1
#define DECLSPEC_EXPORT
#define STATUS_SUCCESS 0u
#define STATUS_NOT_IMPLEMENTED 0xc0000002u
#define STATUS_MUTANT_NOT_OWNED 0xc0000046u
#define STATUS_INVALID_HANDLE 0xc0000008u
#define STATUS_OBJECT_TYPE_MISMATCH 0xc0000024u
#define STATUS_ACCESS_DENIED 0xc0000022u
#define SYNCHRONIZE 0x100000u
#define TERMINATED 2
#define REQ_select 1
#define REQ_release_mutex 2
struct list { struct list *next, *prev; };
static void list_init(struct list *l) { l->next=l->prev=l; }
static int list_empty(const struct list *l) { return l->next==l; }
static struct list *list_head(struct list *l) { return list_empty(l)?NULL:l->next; }
static void list_add_head(struct list *l,struct list *p)
{ p->next=l->next;p->prev=l;l->next->prev=p;l->next=p; }
static void list_remove(struct list *p) { p->next->prev=p->prev;p->prev->next=p->next;list_init(p); }
#define LIST_ENTRY(p,t,f) ((t *)((char *)(p)-offsetof(t,f)))
#include <stddef.h>
struct object_ops { int tag; };
static const struct object_ops mutex_ops={1},mutex_sync_ops={2},other_ops={3},thread_ops={4};
struct object { const struct object_ops *ops; unsigned refs; int destroyed; struct list wait_queue; };
struct handle_entry { struct object *ptr; unsigned access; };
struct process { unsigned suspend; struct handle_entry handles[32]; };
struct context { int cooperative; };
struct thread {
    struct object obj; struct process *process; unsigned id; client_ptr_t teb;
    int state,reply_fd; unsigned req_toread,reply_towrite; void *req_data,*reply_data,*wait;
    struct context *context; unsigned suspend,bypass_proc_suspend,error;
    struct list mutex_list,system_apc,user_apc;
    int ps5_last_req; uint64_t ps5_last_req_time;
};
struct mutex_sync { struct object obj;struct thread *owner;unsigned count;int abandoned;struct list entry; };
struct mutex { struct object obj; struct object *sync; };
static struct thread threads[16];
static struct thread *current;
static unsigned global_error;
static int inprocess_direct_ok=1,debug_level;
static pthread_mutex_t inprocess_server_mutex=PTHREAD_MUTEX_INITIALIZER;
static uint64_t monotonic_time;
static void set_error(unsigned e) { global_error=e;if(current)current->error=e; }
static void clear_error(void) { set_error(0); }
static void *grab_object(void *p) { struct object *o=p;assert(!o->destroyed&&o->refs);o->refs++;return p; }
static void release_object(void *p) {
    struct object *o=p;assert(!o->destroyed&&o->refs);
    if(!--o->refs){o->destroyed=1;if(o->ops==&mutex_ops)release_object(((struct mutex *)p)->sync);
        if(o->ops==&mutex_sync_ops)assert(!((struct mutex_sync *)p)->count);}
}
static void wake_up(struct object *o,int max) { (void)max;assert(list_empty(&o->wait_queue)); }
static void abandon_inproc_mutexes(unsigned tid) { (void)tid; }
static struct object *get_magic_handle(obj_handle_t h) { (void)h;return NULL; }
static struct handle_entry *get_handle(struct process *p,obj_handle_t h)
{ return h<32 && p->handles[h].ptr ? &p->handles[h] : NULL; }
static struct thread *get_thread_from_id(unsigned tid) {
    if(!tid||tid>16){set_error(STATUS_INVALID_HANDLE);return NULL;}
    return grab_object(&threads[tid-1]);
}
static void set_current_time(void) { monotonic_time++; }
/*AUTHORITY*/
/*PATCH_SERVER*/
struct thread_data { unsigned tid; };
static _Thread_local struct thread_data data;
static struct thread_data *get_thread_data(void) { return &data; }
static client_ptr_t inprocess_teb(void) { return threads[data.tid-1].teb; }
static obj_handle_t wine_server_obj_handle(HANDLE h) { return (obj_handle_t)(uintptr_t)h; }
static sigset_t server_block_set;
static int (*try_server_mutex)(unsigned,client_ptr_t,obj_handle_t,int,unsigned *,unsigned *);
static const char *config_dir;
/*PATCH_CLIENT*/
static void switch_contract(void) {
    char *path;assert(asprintf(&path,"%s/pw_mutex_fast",config_dir)>0);
    assert(!unsetenv("WINE_PS5_MUTEX_FAST"));assert(!server_mutex_fast_enabled());
    const char *values[]={"", "0", "1", "1\n", "11", "1\nextra", "on", "1\r\n"};
    for(unsigned i=0;i<sizeof(values)/sizeof(*values);i++){
        FILE *f=fopen(path,"w");assert(f);assert(fputs(values[i],f)>=0);assert(!fclose(f));
        assert(server_mutex_fast_enabled()==(i==2||i==3));
        assert(!setenv("WINE_PS5_MUTEX_FAST","0",1));assert(!server_mutex_fast_enabled());
        assert(!setenv("WINE_PS5_MUTEX_FAST","1",1));assert(server_mutex_fast_enabled());
        assert(!setenv("WINE_PS5_MUTEX_FAST","on",1));assert(!server_mutex_fast_enabled());
        assert(!unsetenv("WINE_PS5_MUTEX_FAST"));
    }
    assert(!unlink(path));free(path);assert(!server_mutex_fast_enabled());
    puts("native switch model: prefix OFF/ON, invalid values and explicit environment precedence PASS");
}
static void obj_init(struct object *o,const struct object_ops *ops)
{ memset(o,0,sizeof(*o));o->ops=ops;o->refs=1;list_init(&o->wait_queue); }
static void init(void) {
    memset(threads,0,sizeof(threads));
    for(unsigned i=0;i<16;i++){
        struct thread *t=&threads[i];obj_init(&t->obj,&thread_ops);t->id=i+1;
        t->teb=0x1000+i*0x100;t->reply_fd=1;
        list_init(&t->mutex_list);list_init(&t->system_apc);list_init(&t->user_apc);
    }
    data.tid=1;try_server_mutex=pw_wineserver_try_fast_mutex;
    sigemptyset(&server_block_set);sigaddset(&server_block_set,SIGUSR1);
}
static void make_mutex(struct mutex *m,struct mutex_sync *s,struct process *p,unsigned h)
{ memset(m,0,sizeof(*m));memset(s,0,sizeof(*s));obj_init(&m->obj,&mutex_ops);
  obj_init(&s->obj,&mutex_sync_ops);list_init(&s->entry);m->sync=&s->obj;
  p->handles[h]=(struct handle_entry){&m->obj,SYNCHRONIZE}; }
static unsigned call(unsigned h,int release,LONG *prev)
{ return server_try_fast_mutex((HANDLE)(uintptr_t)h,release,NULL,prev); }
static void expect_fallback(struct mutex_sync *s,unsigned h,int release) {
    struct mutex_sync before=*s;unsigned old_error=threads[0].error=0x1234;
    global_error=0x5678;
    LONG prev=99;assert(call(h,release,&prev)==STATUS_NOT_IMPLEMENTED);
    assert(!memcmp(&before,s,sizeof(before)));assert(prev==99);
    assert(threads[0].error==old_error&&global_error==0x5678);assert(!current);
    assert(!pthread_mutex_trylock(&inprocess_server_mutex));pthread_mutex_unlock(&inprocess_server_mutex);
}
static void semantics(void) {
    struct process p={0};struct mutex m;struct mutex_sync s;struct thread *t=&threads[0];
    t->process=&p;threads[1].process=&p;make_mutex(&m,&s,&p,1);
    assert(call(1,0,NULL)==0&&s.count==1&&s.owner==t&&s.obj.refs==2);
    assert(!list_empty(&t->mutex_list));
    assert(call(1,0,NULL)==0&&s.count==2&&s.obj.refs==2);
    LONG prev=99;assert(call(1,1,&prev)==0&&prev==-1&&s.count==1);
    assert(call(1,1,&prev)==0&&prev==0&&!s.count&&!s.owner&&s.obj.refs==1);
    assert(list_empty(&t->mutex_list));expect_fallback(&s,1,1);
    expect_fallback(&s,31,0); /* invalid handle */
    p.handles[1].access=0;expect_fallback(&s,1,0);p.handles[1].access=SYNCHRONIZE;
    assert(call(1,0,NULL)==0);p.handles[1].access=0;
    assert(call(1,1,NULL)==0);p.handles[1].access=SYNCHRONIZE; /* Wine release checks access 0 */
    struct object other;obj_init(&other,&other_ops);p.handles[2]=(struct handle_entry){&other,SYNCHRONIZE};
    expect_fallback(&s,2,0);assert(other.refs==1);
    s.obj.ops=&other_ops;expect_fallback(&s,1,0);s.obj.ops=&mutex_sync_ops;
    s.abandoned=1;expect_fallback(&s,1,0);s.abandoned=0;
    struct list waiter;list_init(&waiter);list_add_head(&s.obj.wait_queue,&waiter);
    expect_fallback(&s,1,0);list_remove(&waiter);
    current=&threads[1];do_grab(&s,current);current=NULL;
    expect_fallback(&s,1,0);expect_fallback(&s,1,1);
    current=&threads[1];do_release(&s,current,1);current=NULL;
    assert(call(1,0,NULL)==0);list_add_head(&s.obj.wait_queue,&waiter);
    expect_fallback(&s,1,0);expect_fallback(&s,1,1);list_remove(&waiter);
    s.count=~0u;expect_fallback(&s,1,0);s.count=1;assert(call(1,1,NULL)==0);
    /* Suspension/APC/context/nested-wait branches must not acquire or release. */
    assert(call(1,0,NULL)==0);
#define BOTH() do { expect_fallback(&s,1,0);expect_fallback(&s,1,1); } while(0)
    t->wait=&p;BOTH();t->wait=NULL;
    struct context ctx={0};t->context=&ctx;BOTH();t->context=NULL;
    t->suspend=1;BOTH();t->suspend=0;
    p.suspend=1;BOTH();t->bypass_proc_suspend=1;assert(call(1,0,NULL)==0);
    assert(call(1,1,NULL)==0);t->bypass_proc_suspend=0;p.suspend=0;
    list_add_head(&t->system_apc,&waiter);BOTH();list_remove(&waiter);
    list_add_head(&t->user_apc,&waiter);BOTH();list_remove(&waiter);
    debug_level=1;BOTH();debug_level=0;inprocess_direct_ok=0;BOTH();inprocess_direct_ok=1;
    t->state=TERMINATED;BOTH();t->state=0;t->reply_fd=0;BOTH();t->reply_fd=1;
    t->req_toread=1;BOTH();t->req_toread=0;t->reply_towrite=1;BOTH();t->reply_towrite=0;
    t->req_data=&p;BOTH();t->req_data=NULL;t->reply_data=&p;BOTH();t->reply_data=NULL;
    unsigned status=77,previous=88;
    assert(pw_wineserver_try_fast_mutex(1,t->teb+1,1,0,&status,&previous)==1);
    assert(pw_wineserver_try_fast_mutex(99,t->teb,1,0,&status,&previous)==1);
    assert(status==77&&previous==88&&s.count==1);
    assert(pw_wineserver_try_fast_mutex(1,t->teb,1,2,&status,&previous)==1);
    pthread_mutex_lock(&inprocess_server_mutex);
    assert(call(1,0,NULL)==STATUS_NOT_IMPLEMENTED);pthread_mutex_unlock(&inprocess_server_mutex);
    try_server_mutex=NULL;BOTH();try_server_mutex=pw_wineserver_try_fast_mutex;
    assert(call(1,1,NULL)==0);
    /* Readiness precedes a valid expired/zero/relative timeout on this path. */
    for(int64_t value=-1;value<=1;value++){
        LARGE_INTEGER timeout={value};assert(server_try_fast_mutex((HANDLE)1,0,&timeout,NULL)==0);
        assert(call(1,1,NULL)==0);
    }
    /* Aliases share server ownership. Final handle close keeps the owned sync alive. */
    p.handles[3]=(struct handle_entry){grab_object(&m),SYNCHRONIZE};
    assert(call(1,0,NULL)==0&&call(3,0,NULL)==0&&s.count==2);
    release_object(p.handles[1].ptr);p.handles[1].ptr=NULL;
    expect_fallback(&s,1,0);assert(call(3,1,NULL)==0&&s.count==1);
    release_object(p.handles[3].ptr);p.handles[3].ptr=NULL;
    assert(m.obj.destroyed&&s.obj.refs==1&&!s.obj.destroyed);
    current=t;abandon_mutexes(t);current=NULL;
    assert(s.obj.destroyed&&s.abandoned&&!s.count&&!s.owner&&list_empty(&t->mutex_list));
    /* Reused numeric handle resolves the replacement object, not a cached pointer. */
    struct mutex replacement;struct mutex_sync r;make_mutex(&replacement,&r,&p,1);
    assert(call(1,0,NULL)==0&&r.owner==t&&call(1,1,NULL)==0);
    release_object(p.handles[1].ptr);p.handles[1].ptr=NULL;
    assert(r.obj.destroyed);
    sigset_t mask;pthread_sigmask(SIG_SETMASK,NULL,&mask);assert(!sigismember(&mask,SIGUSR1));
    puts("native mutex model: ownership/recursion/refs/access/aliases/close/reuse/abandonment and eligibility PASS");
}
static atomic_uint inside,entries,fast_count,slow_count;
static struct mutex *shared_mutex;
static unsigned model_slow(unsigned tid,int release) {
    unsigned previous=0;pthread_mutex_lock(&inprocess_server_mutex);current=&threads[tid-1];
    int handled=try_fast_mutex(current,1,release,&previous);current=NULL;
    pthread_mutex_unlock(&inprocess_server_mutex);return handled?0:STATUS_NOT_IMPLEMENTED;
}
static void *worker(void *arg) {
    data.tid=(unsigned)(uintptr_t)arg;
    for(unsigned i=0;i<2000;i++){
        unsigned attempts=0;
        for(;;){
            unsigned result=call(1,0,NULL);
            if(!result){atomic_fetch_add(&fast_count,1);break;}
            assert(result==STATUS_NOT_IMPLEMENTED);
            if(!model_slow(data.tid,0)){atomic_fetch_add(&slow_count,1);break;}
            assert(++attempts<1000000);sched_yield();
        }
        assert(atomic_fetch_add(&inside,1)==0);
        assert(((struct mutex_sync *)shared_mutex->sync)->owner==&threads[data.tid-1]);
        atomic_fetch_add(&entries,1);assert(atomic_fetch_sub(&inside,1)==1);
        unsigned result=call(1,1,NULL);
        if(result){assert(result==STATUS_NOT_IMPLEMENTED);assert(!model_slow(data.tid,1));}
    }
    return NULL;
}
static void concurrency(void) {
    for(unsigned n=2;n<=16;n*=2){
        struct process p={0};struct mutex m;struct mutex_sync s;make_mutex(&m,&s,&p,1);
        for(unsigned i=0;i<n;i++)threads[i].process=&p;
        atomic_store(&entries,0);atomic_store(&inside,0);shared_mutex=&m;
        pthread_t ids[16];for(unsigned i=0;i<n;i++)assert(!pthread_create(&ids[i],NULL,worker,(void *)(uintptr_t)(i+1)));
        for(unsigned i=0;i<n;i++)assert(!pthread_join(ids[i],NULL));
        assert(atomic_load(&entries)==n*2000&&!s.count&&!s.owner&&s.obj.refs==1&&m.obj.refs==1);
        for(unsigned i=0;i<n;i++)assert(threads[i].obj.refs==1&&list_empty(&threads[i].mutex_list));
        release_object(&m);
    }
    printf("native bounded model: 2/4/8/16-thread exclusion PASS, fast=%u fallback-model=%u\n",
           atomic_load(&fast_count),atomic_load(&slow_count));
}
int main(int argc,char **argv) { assert(argc==2);config_dir=argv[1];switch_contract();init();semantics();concurrency();return 0; }
'''


def main():
    sources = added_source()
    server = "\n\n".join(function(sources[path], name) for path, name in (
        ("server/mutex.c", "try_fast_mutex"),
        ("server/thread.c", "thread_can_fast_mutex"),
        ("server/request.c", "pw_wineserver_try_fast_mutex")))
    client = "\n\n".join(function(sources["dlls/ntdll/unix/server.c"], name)
                           for name in ("server_mutex_fast_enabled", "server_try_fast_mutex"))
    code = HARNESS.replace("/*AUTHORITY*/", (ROOT / "tests/fixtures/wine_mutex_authority.c").read_text())
    code = code.replace("/*PATCH_SERVER*/", server).replace("/*PATCH_CLIENT*/", client)
    # Registration is default-off and missing exports/disabled direct requests fall back.
    binding = sources["dlls/ntdll/unix/server.c"]
    assert 'if (call_server_direct && server_mutex_fast_enabled())' in binding
    assert 'getenv( "WINE_PS5_MUTEX_FAST" )' in binding
    assert 'if (!alertable &&' in sources["dlls/ntdll/unix/sync.c"]
    assert "pw_wineserver_call_direct pw_wineserver_try_fast_mutex" in (ROOT / "tools/build_wine_ps5.sh").read_text()
    with tempfile.TemporaryDirectory(prefix="pw-mutex-model-") as directory:
        directory = Path(directory)
        source = directory / "check.c"
        source.write_text(code)
        compiler = shlex.split(os.environ.get("CC", "cc"))
        flags = shlex.split(os.environ.get("CFLAGS", "-O2 -Wall -Wextra -Werror"))
        subprocess.run([*compiler, "-std=c11", *flags, "-pthread", str(source), "-o", str(directory / "check")], check=True)
        subprocess.run([str(directory / "check"), str(directory)], check=True, timeout=30)
    print("Full Wine/PS5 wait queues, delivery, loader lifetime and speed: owner validation pending")


if __name__ == "__main__":
    main()
