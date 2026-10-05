/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Exact client/ABI bodies; local legal metadata, no Wine/signals/faults. */
#define _GNU_SOURCE
#include "ps5_sync_backend.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#define WINE_INPROCESS_SERVER 1
#define STATUS_SUCCESS 0u
#define STATUS_NOT_IMPLEMENTED 0xc0000002u
#define SYNCHRONIZE 0x100000u
#define EVENT_MODIFY_STATE 2u
#define SEMAPHORE_MODIFY_STATE 2u
#define TERMINATED 1
#define DECLSPEC_EXPORT
typedef uint32_t obj_handle_t;
typedef uintptr_t HANDLE;
typedef int64_t timeout_t;
typedef union { int64_t QuadPart; } LARGE_INTEGER;
struct thread_data { unsigned tid; };
static _Thread_local struct thread_data fixture_thread = {.tid=1};
static struct thread_data *get_thread_data(void) { return &fixture_thread; }
static obj_handle_t wine_server_obj_handle(HANDLE handle) { return (obj_handle_t)handle; }
static uint64_t inprocess_teb(void) { return (uint64_t)(uintptr_t)&fixture_thread; }
static pthread_mutex_t fd_cache_mutex=PTHREAD_MUTEX_INITIALIZER;
static _Atomic unsigned cold_calls,sections;
static void *pages[8]; static unsigned page_count;
static int fail_page;
static void *anon_mmap_alloc(size_t size,int prot) {
    assert(size==65536 && prot==(PROT_READ|PROT_WRITE));
    if (fail_page) return MAP_FAILED;
    assert(page_count<8); void *p=calloc(1,size);
    if (!p) return MAP_FAILED;
    pages[page_count++]=p; return p;
}
static void server_enter_uninterrupted_section(pthread_mutex_t *lock,sigset_t *set) {
    (void)set; assert(!pthread_mutex_lock(lock)); atomic_fetch_add(&sections,1);
}
static void server_leave_uninterrupted_section(pthread_mutex_t *lock,sigset_t *set) {
    (void)set; assert(!pthread_mutex_unlock(lock));
}
struct object { unsigned refs,type; };
struct thread { struct object obj; uint64_t teb; unsigned id,state,error,req_toread,reply_towrite;
    int reply_fd; void *req_data,*reply_data,*process; };
struct node { struct object obj; struct pw_sync_word word; unsigned handle,access,eligible; };
static struct node nodes[200]; static unsigned node_count;
static struct thread server_threads[64];
static struct thread *current;
static int inprocess_direct_ok=1,debug_level,allow_thread=1,retry_once;
static unsigned global_error=7,server_depth,missing_tid,gate_checks;
static pthread_mutex_t inprocess_server_mutex=PTHREAD_MUTEX_INITIALIZER;
static struct thread *get_thread_from_id(unsigned tid) {
    assert(tid<64); struct thread *t=&server_threads[tid];
    if (tid==missing_tid) { global_error=11; return NULL; }
    if (!t->id) { t->id=tid; t->teb=inprocess_teb(); t->reply_fd=1; t->error=9; t->obj.type=1; t->process=t; }
    ++t->obj.refs; return t;
}
static void release_object(void *pointer) {
    struct object *obj=pointer; assert(obj->refs); --obj->refs;
}
static int thread_can_fast_mutex(struct thread *t) { (void)t; return allow_thread; }
static void clear_error(void) { global_error=0; assert(current); current->error=0; }
static void ps5_mutex_server_begin(void) { ++server_depth; }
static void ps5_mutex_server_end(void) { assert(server_depth); --server_depth; }
static void ps5_mutex_server_reconcile(void) { assert(!server_depth); }
static struct object *get_handle_obj(void *process,unsigned handle,unsigned access,void *type) {
    assert(current && process==current && access==0 && !type && server_depth==1);
    assert(current->error==0 && global_error==0); atomic_fetch_add(&cold_calls,1);
    for (unsigned i=0;i<node_count;i++) if (nodes[i].handle==handle) {
        ++nodes[i].obj.refs; return &nodes[i].obj;
    }
    global_error=17; current->error=18; return NULL;
}
static unsigned get_handle_access(void *process,unsigned handle) {
    assert(process==current);
    for (unsigned i=0;i<node_count;i++) if (nodes[i].handle==handle) return nodes[i].access;
    assert(0); return 0;
}
static int get_word(struct thread *thread,struct object *obj,struct pw_sync_word **word,int semaphore) {
    assert(thread==current && server_depth==1);
    struct node *n=(struct node *)obj;
    if (!n->eligible || (n->word.kind==PW_SYNC_SEMAPHORE)!=semaphore) return 0;
    if (retry_once) { retry_once=0; return -1; }
    *word=&n->word; return 1;
}
static int ps5_get_event_word(struct thread *t,struct object *o,struct pw_sync_word **w) { return get_word(t,o,w,0); }
static int ps5_get_semaphore_word(struct thread *t,struct object *o,struct pw_sync_word **w) { return get_word(t,o,w,1); }
#include "shared_sync_server_abi.inc"
static const struct pw_sync_backend *shared_sync_backend;
#include "shared_sync_client.inc"
static const char *config_dir;
#include "shared_sync_switch.inc"

static void write_switch(const char *value) {
    char *path; assert(asprintf(&path,"%s/pw_sync_shared",config_dir)>0);
    FILE *f=fopen(path,"w"); assert(f && fputs(value,f)>=0 && !fclose(f)); free(path);
}
static void test_switch(void) {
    const char *values[]={"","0","1","1\n","11","1\nextra","on","1\r\n"};
    assert(!unsetenv("WINE_PS5_SYNC_SHARED") && !server_shared_sync_enabled());
    assert(!setenv("WINE_PS5_MUTEX_SHARED","1",1) && !server_shared_sync_enabled());
    for (unsigned i=0;i<8;i++) {
        write_switch(values[i]); assert(server_shared_sync_enabled()==(i==2 || i==3));
        assert(!setenv("WINE_PS5_SYNC_SHARED","0",1) && !server_shared_sync_enabled());
        assert(!setenv("WINE_PS5_SYNC_SHARED","1",1) && server_shared_sync_enabled());
        assert(!setenv("WINE_PS5_SYNC_SHARED","on",1) && !server_shared_sync_enabled());
        assert(!unsetenv("WINE_PS5_SYNC_SHARED"));
    }
    char *path; assert(asprintf(&path,"%s/pw_sync_shared",config_dir)>0); assert(!remove(path)); free(path);
    assert(!unsetenv("WINE_PS5_MUTEX_SHARED") && !server_shared_sync_enabled());
}
static struct node *node(unsigned handle,unsigned kind,unsigned initial,unsigned max,unsigned access,int eligible) {
    assert(node_count<200); struct node *n=&nodes[node_count++];
    n->obj.type=2; n->handle=handle; n->access=access; n->eligible=eligible;
    pw_sync_word_init(&n->word,kind,initial,max); assert(pw_sync_word_publish(&n->word,initial)); return n;
}
static void expect_retry(const struct pw_sync_backend *api,unsigned version,uint64_t teb) {
    struct pw_sync_word *word=&nodes[0].word; unsigned access=47,before=cold_calls;
    struct thread *saved_current=current;
    assert(api->get_word(version,1,teb,4,&word,&access)==PW_SYNC_LOOKUP_RETRY);
    assert(word==&nodes[0].word && access==47 && cold_calls==before);
    assert(global_error==7 && !server_depth && current==saved_current);
    assert(!server_threads[1].obj.refs && server_threads[1].error==9);
    assert(pw_sync_word_load(&nodes[0].word)==1); ++gate_checks;
}
static void test_gates(const struct pw_sync_backend *api) {
    struct thread *t=get_thread_from_id(1); release_object(t); uint64_t teb=inprocess_teb(),saved=t->teb; int value;
    expect_retry(api,PW_SYNC_BACKEND_VERSION+1,teb);
    inprocess_direct_ok=0; expect_retry(api,PW_SYNC_BACKEND_VERSION,teb); inprocess_direct_ok=1;
    assert(!pthread_mutex_lock(&inprocess_server_mutex)); expect_retry(api,PW_SYNC_BACKEND_VERSION,teb);
    assert(!pthread_mutex_unlock(&inprocess_server_mutex));
    current=t; expect_retry(api,PW_SYNC_BACKEND_VERSION,teb); current=NULL;
    debug_level=1; expect_retry(api,PW_SYNC_BACKEND_VERSION,teb); debug_level=0;
    missing_tid=1; expect_retry(api,PW_SYNC_BACKEND_VERSION,teb); missing_tid=0;
    t->teb=0; expect_retry(api,PW_SYNC_BACKEND_VERSION,teb); t->teb=saved;
    expect_retry(api,PW_SYNC_BACKEND_VERSION,0);
    t->state=TERMINATED; expect_retry(api,PW_SYNC_BACKEND_VERSION,teb); t->state=0;
    t->reply_fd=0; expect_retry(api,PW_SYNC_BACKEND_VERSION,teb); t->reply_fd=1;
    t->req_toread=1; expect_retry(api,PW_SYNC_BACKEND_VERSION,teb); t->req_toread=0;
    t->reply_towrite=1; expect_retry(api,PW_SYNC_BACKEND_VERSION,teb); t->reply_towrite=0;
    t->req_data=&value; expect_retry(api,PW_SYNC_BACKEND_VERSION,teb); t->req_data=NULL;
    t->reply_data=&value; expect_retry(api,PW_SYNC_BACKEND_VERSION,teb); t->reply_data=NULL;
    allow_thread=0; expect_retry(api,PW_SYNC_BACKEND_VERSION,teb); allow_thread=1;
    assert(gate_checks==15);
}
static void success(HANDLE h,unsigned op,unsigned count,unsigned old) {
    unsigned previous=99;
    assert(server_try_shared_sync(h,op,count,NULL,&previous)==STATUS_SUCCESS);
    assert(previous==(op==PW_SYNC_WAIT ? 99 : old));
}
static void fallback(HANDLE h,unsigned op,unsigned count) {
    unsigned previous=99; assert(server_try_shared_sync(h,op,count,NULL,&previous)==STATUS_NOT_IMPLEMENTED);
    assert(previous==99);
}
static unsigned protected_counter;
static void *worker(void *arg) {
    fixture_thread.tid=(unsigned)(uintptr_t)arg;
    for (unsigned i=0;i<3000;i++) {
        while (server_try_shared_sync(16,PW_SYNC_WAIT,0,NULL,NULL)!=STATUS_SUCCESS) sched_yield();
        ++protected_counter;
        assert(server_try_shared_sync(16,PW_SYNC_RELEASE,1,NULL,NULL)==STATUS_SUCCESS);
    }
    return NULL;
}
int main(int argc,char **argv) {
    assert(argc==2); config_dir=argv[1]; test_switch();
    const struct pw_sync_backend *api=pw_wineserver_sync_backend(PW_SYNC_BACKEND_VERSION);
    assert(api && !pw_wineserver_sync_backend(PW_SYNC_BACKEND_VERSION+1)); shared_sync_backend=api;
    assert(pw_sync_backend_valid(api) && !pw_sync_backend_valid(NULL));
    struct pw_sync_backend wrong=*api;
    wrong.version++; assert(!pw_sync_backend_valid(&wrong)); wrong=*api;
    wrong.size--; assert(!pw_sync_backend_valid(&wrong)); wrong=*api;
    wrong.word_size--; assert(!pw_sync_backend_valid(&wrong)); wrong=*api;
    wrong.pointer_size--; assert(!pw_sync_backend_valid(&wrong)); wrong=*api;
    wrong.ready=NULL; assert(!pw_sync_backend_valid(&wrong)); wrong=*api;
    wrong.get_word=NULL; assert(!pw_sync_backend_valid(&wrong));
    struct node *auto_event=node(4,PW_SYNC_AUTO_EVENT,1,1,SYNCHRONIZE|2,1); test_gates(api);
    success(4,PW_SYNC_WAIT,0,0); fallback(4,PW_SYNC_WAIT,0); success(4,PW_SYNC_SET,0,0);
    struct node *manual_event=node(8,PW_SYNC_MANUAL_EVENT,1,1,SYNCHRONIZE|2,1);
    success(8,PW_SYNC_WAIT,0,0); success(8,PW_SYNC_WAIT,0,0); assert(pw_sync_word_load(&manual_event->word)==1);
    success(8,PW_SYNC_RESET,0,1); fallback(8,PW_SYNC_WAIT,0); success(8,PW_SYNC_SET,0,0);
    struct node *sem=node(12,PW_SYNC_SEMAPHORE,2,3,SYNCHRONIZE|2,1);
    success(12,PW_SYNC_RELEASE,1,2); fallback(12,PW_SYNC_RELEASE,1); fallback(12,PW_SYNC_RELEASE,0);
    fallback(12,PW_SYNC_RELEASE,UINT32_MAX); fallback(12,99,1);
    fallback(12,PW_SYNC_SET,0); fallback(4,PW_SYNC_RELEASE,1); assert(pw_sync_word_load(&sem->word)==3);
    success(12,PW_SYNC_WAIT,0,0); success(12,PW_SYNC_WAIT,0,0); success(12,PW_SYNC_WAIT,0,0); fallback(12,PW_SYNC_WAIT,0);
    success(12,PW_SYNC_RELEASE,2,0);
    unsigned cold_before=cold_calls,sections_before=sections;
    for (unsigned i=0;i<6000;i++) {
        success(4,PW_SYNC_WAIT,0,0); success(4,PW_SYNC_SET,0,0);
        success(8,PW_SYNC_RESET,0,1); success(8,PW_SYNC_SET,0,0);
        success(12,PW_SYNC_WAIT,0,0); success(12,PW_SYNC_RELEASE,1,1);
    }
    assert(cold_calls==cold_before && sections==sections_before);
    /* Every access combination is independent of object kind and state. */
    for (unsigned kind=1;kind<=3;kind++) for (unsigned bits=0;bits<4;bits++) {
        unsigned h=100+4*((kind-1)*4+bits),access=(bits&1 ? SYNCHRONIZE:0)|(bits&2 ? 2:0);
        struct node *n=node(h,kind,1,kind==3?2:1,access,1);
        if (bits&1) success(h,PW_SYNC_WAIT,0,0); else fallback(h,PW_SYNC_WAIT,0);
        unsigned old=kind==2||!(bits&1)?1:0;
        if (bits&2) success(h,kind==3?PW_SYNC_RELEASE:PW_SYNC_SET,1,old);
        else { fallback(h,kind==3?PW_SYNC_RELEASE:PW_SYNC_SET,1); assert(pw_sync_word_load(&n->word)==old); }
    }
    for (unsigned i=0;i<80;i++) { unsigned h=1000+4*i; node(h,1,0,1,0,0); fallback(h,PW_SYNC_WAIT,0); }
    cold_before=cold_calls; sections_before=sections;
    for (unsigned pass=0;pass<25;pass++) for (unsigned i=0;i<80;i++) fallback(1000+4*i,PW_SYNC_WAIT,0);
    assert(cold_calls==cold_before && sections==sections_before);
    node(1400,1,1,1,SYNCHRONIZE,1); retry_once=1; fallback(1400,PW_SYNC_WAIT,0);
    assert(!__atomic_load_n(shared_sync_slot(1400,0),__ATOMIC_ACQUIRE)); success(1400,PW_SYNC_WAIT,0,0);
    fallback(1404,PW_SYNC_WAIT,0); assert(!__atomic_load_n(shared_sync_slot(1404,0),__ATOMIC_ACQUIRE));
    node(1404,1,1,1,SYNCHRONIZE,1); success(1404,PW_SYNC_WAIT,0,0);
    unsigned high=4*(PW_SYNC_CACHE_SLOTS+7)+4; node(high,1,1,1,SYNCHRONIZE,1);
    fail_page=1; fallback(high,PW_SYNC_WAIT,0); assert(!shared_sync_slot(high,0));
    fail_page=0; success(high,PW_SYNC_WAIT,0,0); assert(page_count==1);
    assert(!shared_sync_slot(0,1) && !shared_sync_slot(5,1) && !shared_sync_slot(UINT32_MAX,1));
    uint32_t snapshot; assert(pw_sync_word_freeze(&auto_event->word,&snapshot) && snapshot==1);
    cold_before=cold_calls; sections_before=sections; fallback(4,PW_SYNC_WAIT,0); fallback(4,PW_SYNC_SET,0);
    assert(cold_calls==cold_before && sections==sections_before); assert(pw_sync_word_publish(&auto_event->word,snapshot));
    inprocess_direct_ok=0; fallback(4,PW_SYNC_WAIT,0); assert(pw_sync_word_load(&auto_event->word)==1);
    inprocess_direct_ok=1; success(4,PW_SYNC_WAIT,0,0);
    assert(!pthread_mutex_lock(&fd_cache_mutex)); pw_sync_word_freeze(&auto_event->word,&snapshot);
    auto_event->handle=0; server_clear_shared_sync_slot(4); node(4,2,1,1,SYNCHRONIZE,1);
    assert(!pthread_mutex_unlock(&fd_cache_mutex)); success(4,PW_SYNC_WAIT,0,0);
    assert(pw_sync_word_load(&auto_event->word)&PW_SYNC_WORD_SLOW);
    struct node *shared=node(16,3,1,1,SYNCHRONIZE|2,1);
    pthread_t threads[4];
    for (unsigned i=0;i<4;i++) assert(!pthread_create(&threads[i],NULL,worker,(void *)(uintptr_t)(10+i)));
    for (unsigned i=0;i<4;i++) assert(!pthread_join(threads[i],NULL));
    assert(protected_counter==12000 && pw_sync_word_load(&shared->word)==1);
    assert(global_error==7 && !current && !server_depth);
    for (unsigned i=0;i<64;i++) assert(!server_threads[i].obj.refs);
    for (unsigned i=0;i<node_count;i++) assert(!nodes[i].obj.refs);
    for (unsigned i=0;i<page_count;i++) free(pages[i]);
    puts("PASS: ABI/context/access/strict switches, 36000 warm operations, 80 exact negatives, retry/close/reuse, 12000 concurrent semaphore sections");
    return 0;
}
