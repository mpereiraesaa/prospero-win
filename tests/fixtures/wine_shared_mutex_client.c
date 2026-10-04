/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Actual client bodies, native local metadata and ordinary legal lifecycle.
 * No Wine, application faults, signals or asynchronous termination. */
#define _GNU_SOURCE
#include "ps5_mutex_backend.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define WINE_INPROCESS_SERVER 1
#define STATUS_SUCCESS 0u
#define STATUS_NOT_IMPLEMENTED 0xc0000002u
#define SYNCHRONIZE 0x100000u
typedef uint32_t obj_handle_t;
typedef uintptr_t HANDLE;
typedef int32_t LONG;
typedef int64_t timeout_t;
typedef union { int64_t QuadPart; } LARGE_INTEGER;
struct thread_data { unsigned tid, ps5_mutex_token; };
static _Thread_local struct thread_data fixture_thread={.tid=1};
static struct thread_data *get_thread_data(void) { return &fixture_thread; }
static obj_handle_t wine_server_obj_handle(HANDLE h) { return (obj_handle_t)h; }
static uint64_t inprocess_teb(void) { return (uint64_t)(uintptr_t)&fixture_thread; }
static pthread_mutex_t fd_cache_mutex=PTHREAD_MUTEX_INITIALIZER;
static _Atomic unsigned cold_calls, sections;
static void *pages[8]; static unsigned page_count;
static void *anon_mmap_alloc(size_t size,int prot) {
    assert(prot==(PROT_READ|PROT_WRITE) && page_count<8);
    void *p=calloc(1,size); if (!p) return MAP_FAILED; pages[page_count++]=p; return p;
}
static void server_enter_uninterrupted_section(pthread_mutex_t *lock,sigset_t *set) {
    (void)set; assert(!pthread_mutex_lock(lock)); atomic_fetch_add(&sections,1);
}
static void server_leave_uninterrupted_section(pthread_mutex_t *lock,sigset_t *set) {
    (void)set; assert(!pthread_mutex_unlock(lock));
}
struct node { struct pw_mutex_word word; unsigned handle,kind,access; };
static struct node nodes[100]; static unsigned node_count;
static int ready=1, retry_once;
#define DECLSPEC_EXPORT
#define TERMINATED 1
#define inprocess_direct_ok ready
struct thread { uint64_t teb; unsigned id,state,error,refs,req_toread,reply_towrite;
    int reply_fd; void *req_data,*reply_data; };
static struct thread server_threads[256];
static struct thread *current;
static unsigned global_error=7,server_depth;
static int debug_level;
static unsigned missing_tid,thread_lookups,gate_checks;
static int allow_thread=1;
static pthread_mutex_t inprocess_server_mutex=PTHREAD_MUTEX_INITIALIZER;
static struct thread *get_thread_from_id(unsigned tid) {
    assert(tid<256); struct thread *t=&server_threads[tid];
    ++thread_lookups;
    if (tid==missing_tid) { global_error=11; return NULL; }
    if (!t->id) { t->id=tid; t->teb=inprocess_teb(); t->reply_fd=1; t->error=9; }
    ++t->refs; return t;
}
static int thread_can_fast_mutex(struct thread *t) { (void)t; return allow_thread; }
static void release_object(struct thread *t) { assert(t->refs); --t->refs; }
static void clear_error(void) { global_error=0; if (current) current->error=0; }
static void ps5_mutex_server_begin(void) { ++server_depth; }
static void ps5_mutex_server_end(void) { assert(server_depth); --server_depth; }
static void ps5_mutex_server_reconcile(void) { assert(!server_depth); }
static int mock_get_word(uint32_t version,uint32_t tid,uint64_t teb,uint32_t handle,
                         struct pw_mutex_word **word,uint32_t *token,uint32_t *access) {
    assert(version==PW_MUTEX_BACKEND_VERSION && tid && teb);
    atomic_fetch_add(&cold_calls,1);
    if (retry_once) { retry_once=0; return PW_MUTEX_LOOKUP_RETRY; }
    for (unsigned i=0;i<node_count;i++) if (nodes[i].handle==handle) {
        if (!nodes[i].kind) return PW_MUTEX_LOOKUP_NEGATIVE;
        *word=&nodes[i].word; *token=100+tid; *access=nodes[i].access;
        return PW_MUTEX_LOOKUP_READY;
    }
    return PW_MUTEX_LOOKUP_RETRY;
}
static int ps5_describe_mutex_word(struct thread *t,obj_handle_t handle,
                                   struct pw_mutex_word **word,unsigned *token,unsigned *access) {
    assert(current==t && t->error==0 && global_error==0 && server_depth==1);
    return mock_get_word(PW_MUTEX_BACKEND_VERSION,t->id,t->teb,handle,word,token,access);
}
#include "shared_mutex_server_abi.inc"
static const struct pw_mutex_backend *shared_mutex_backend;
#include "shared_mutex_client.inc"
static const char *config_dir;
#include "shared_mutex_switch.inc"

static void write_switch(const char *name,const char *value) {
    char *path; assert(asprintf(&path,"%s/%s",config_dir,name)>0);
    FILE *f=fopen(path,"w"); assert(f);
    assert(fputs(value,f)>=0 && !fclose(f)); free(path);
}
static void remove_switch(const char *name) {
    char *path; assert(asprintf(&path,"%s/%s",config_dir,name)>0);
    assert(!remove(path)); free(path);
}
static void test_switches(void) {
    const char *envs[]={"WINE_PS5_MUTEX_FAST","WINE_PS5_MUTEX_SHARED"};
    const char *files[]={"pw_mutex_fast","pw_mutex_shared"};
    const char *values[]={"","0","1","1\n","11","1\nextra","on","1\r\n"};
    for (unsigned lane=0;lane<2;lane++) {
        int default_enabled=lane==1;
        assert(!unsetenv(envs[lane]));
        assert(server_mutex_switch_enabled(envs[lane],files[lane],default_enabled)==default_enabled);
        for (unsigned i=0;i<8;i++) {
            write_switch(files[lane],values[i]);
            assert(server_mutex_switch_enabled(envs[lane],files[lane],default_enabled)==(i==2 || i==3));
            assert(!setenv(envs[lane],"0",1));
            assert(!server_mutex_switch_enabled(envs[lane],files[lane],default_enabled));
            assert(!setenv(envs[lane],"1",1));
            assert(server_mutex_switch_enabled(envs[lane],files[lane],default_enabled));
            assert(!setenv(envs[lane],"on",1));
            assert(!server_mutex_switch_enabled(envs[lane],files[lane],default_enabled));
            assert(!setenv(envs[lane],"1\n",1));
            assert(!server_mutex_switch_enabled(envs[lane],files[lane],default_enabled));
            assert(!unsetenv(envs[lane]));
        }
        remove_switch(files[lane]);
        assert(server_mutex_switch_enabled(envs[lane],files[lane],default_enabled)==default_enabled);
        char *path; assert(asprintf(&path,"%s/%s",config_dir,files[lane])>0);
        assert(!mkdir(path,0700)); /* fopen succeeds, but fgetc reports a read error */
        assert(!server_mutex_switch_enabled(envs[lane],files[lane],default_enabled));
        assert(!rmdir(path)); free(path);
        assert(asprintf(&path,"%s/%s",config_dir,files[lane])>0);
        assert(!symlink(files[lane],path)); /* open failure other than ENOENT stays off */
        assert(!server_mutex_switch_enabled(envs[lane],files[lane],default_enabled));
        assert(!unlink(path)); free(path);
    }
    write_switch(files[0],"1");
    assert(server_mutex_switch_enabled(envs[0],files[0],0));
    assert(server_mutex_switch_enabled(envs[1],files[1],1));
    write_switch(files[1],"0\n");
    assert(server_mutex_switch_enabled(envs[0],files[0],0));
    assert(!server_mutex_switch_enabled(envs[1],files[1],1));
    write_switch(files[1],"1\n");
    assert(!setenv(envs[0],"0",1));
    assert(!server_mutex_switch_enabled(envs[0],files[0],0));
    assert(server_mutex_switch_enabled(envs[1],files[1],1));
    assert(!unsetenv(envs[0]));
    remove_switch(files[0]); remove_switch(files[1]);
}

static struct node *node(unsigned handle,unsigned kind,unsigned access) {
    assert(node_count<100); struct node *n=&nodes[node_count++];
    n->handle=handle; n->kind=kind; n->access=access; return n;
}
static void expect_lookup_retry(const struct pw_mutex_backend *api,uint32_t version,uint64_t teb) {
    struct pw_mutex_word *word=&nodes[0].word;
    uint32_t token=23,access=47;
    unsigned before=cold_calls;
    struct thread *saved_current=current;
    assert(api->get_word(version,1,teb,4,&word,&token,&access)==PW_MUTEX_LOOKUP_RETRY);
    assert(word==&nodes[0].word && token==23 && access==47);
    assert(cold_calls==before && global_error==7 && !server_depth);
    assert(current==saved_current && !server_threads[1].refs && server_threads[1].error==9);
    assert(!pw_mutex_word_load(&nodes[0].word));
    ++gate_checks;
}
static void test_lookup_gates(const struct pw_mutex_backend *api) {
    struct thread *t=get_thread_from_id(1); release_object(t);
    unsigned lookups=thread_lookups;
    uint64_t teb=inprocess_teb(),saved_teb=t->teb;
    int data;
    expect_lookup_retry(api,PW_MUTEX_BACKEND_VERSION+1,teb);
    __atomic_store_n(&ready,0,__ATOMIC_RELEASE);
    expect_lookup_retry(api,PW_MUTEX_BACKEND_VERSION,teb);
    __atomic_store_n(&ready,1,__ATOMIC_RELEASE);
    assert(!pthread_mutex_lock(&inprocess_server_mutex));
    expect_lookup_retry(api,PW_MUTEX_BACKEND_VERSION,teb);
    assert(!pthread_mutex_unlock(&inprocess_server_mutex));
    current=t; expect_lookup_retry(api,PW_MUTEX_BACKEND_VERSION,teb); current=NULL;
    debug_level=1; expect_lookup_retry(api,PW_MUTEX_BACKEND_VERSION,teb); debug_level=0;
    assert(thread_lookups==lookups);
    missing_tid=1; expect_lookup_retry(api,PW_MUTEX_BACKEND_VERSION,teb); missing_tid=0;
    t->teb=0; expect_lookup_retry(api,PW_MUTEX_BACKEND_VERSION,teb); t->teb=saved_teb;
    expect_lookup_retry(api,PW_MUTEX_BACKEND_VERSION,0);
    /* Fixture state only; no actual thread is terminated or signaled. */
    t->state=TERMINATED; expect_lookup_retry(api,PW_MUTEX_BACKEND_VERSION,teb); t->state=0;
    t->reply_fd=0; expect_lookup_retry(api,PW_MUTEX_BACKEND_VERSION,teb); t->reply_fd=1;
    t->req_toread=1; expect_lookup_retry(api,PW_MUTEX_BACKEND_VERSION,teb); t->req_toread=0;
    t->reply_towrite=1; expect_lookup_retry(api,PW_MUTEX_BACKEND_VERSION,teb); t->reply_towrite=0;
    t->req_data=&data; expect_lookup_retry(api,PW_MUTEX_BACKEND_VERSION,teb); t->req_data=NULL;
    t->reply_data=&data; expect_lookup_retry(api,PW_MUTEX_BACKEND_VERSION,teb); t->reply_data=NULL;
    allow_thread=0; expect_lookup_retry(api,PW_MUTEX_BACKEND_VERSION,teb); allow_thread=1;
    assert(gate_checks==15 && thread_lookups==lookups+10);
}
static void take_release(HANDLE handle) {
    assert(server_try_shared_mutex(handle,0,NULL,NULL)==STATUS_SUCCESS);
    assert(server_try_shared_mutex(handle,1,NULL,NULL)==STATUS_SUCCESS);
}
static unsigned protected_counter;
static void *worker(void *arg) {
    fixture_thread=(struct thread_data){.tid=(unsigned)(uintptr_t)arg};
    for (unsigned i=0;i<3000;i++) {
        while (server_try_shared_mutex(4,0,NULL,NULL)!=STATUS_SUCCESS) sched_yield();
        ++protected_counter;
        assert(server_try_shared_mutex(4,1,NULL,NULL)==STATUS_SUCCESS);
    }
    return NULL;
}
int main(int argc,char **argv) {
    assert(argc==2); config_dir=argv[1]; test_switches();
    /* Native ABI mismatch discovery never invokes metadata or reads a word. */
    const struct pw_mutex_backend *api=pw_wineserver_mutex_backend(PW_MUTEX_BACKEND_VERSION);
    assert(api && !pw_wineserver_mutex_backend(PW_MUTEX_BACKEND_VERSION+1));
    shared_mutex_backend=api;
    assert(pw_mutex_backend_valid(api) && !pw_mutex_backend_valid(NULL));
    struct pw_mutex_backend wrong=*api;
    wrong.version++; assert(!pw_mutex_backend_valid(&wrong)); wrong=*api;
    wrong.size--; assert(!pw_mutex_backend_valid(&wrong)); wrong=*api;
    wrong.word_size--; assert(!pw_mutex_backend_valid(&wrong)); wrong=*api;
    wrong.pointer_size--; assert(!pw_mutex_backend_valid(&wrong)); wrong=*api;
    wrong.ready=NULL; assert(!pw_mutex_backend_valid(&wrong)); wrong=*api;
    wrong.get_word=NULL; assert(!pw_mutex_backend_valid(&wrong));
    struct node *first=node(4,1,SYNCHRONIZE);
    test_lookup_gates(api);
    take_release(4); assert(fixture_thread.ps5_mutex_token==101 && cold_calls==1 && sections==1);
    assert(global_error==7 && server_threads[1].error==9 && !server_threads[1].refs && !current);
    unsigned cold_before=cold_calls, sections_before=sections;
    for (unsigned i=0;i<12000;i++) take_release(4);
    assert(cold_calls==cold_before && sections==sections_before && !pw_mutex_word_load(&first->word));
    /* Recursion and the caller's valid local previous-count storage. */
    LARGE_INTEGER expired={.QuadPart=0}; LONG previous=99;
    assert(server_try_shared_mutex(4,0,&expired,NULL)==STATUS_SUCCESS);
    assert(server_try_shared_mutex(4,0,NULL,NULL)==STATUS_SUCCESS);
    assert(server_try_shared_mutex(4,1,NULL,&previous)==STATUS_SUCCESS && previous==-1);
    assert(server_try_shared_mutex(4,1,NULL,&previous)==STATUS_SUCCESS && previous==0);
    /* Zero wait access cannot acquire, while release keeps Wine access 0. */
    struct node *without_access=node(8,1,0);
    assert(server_try_shared_mutex(8,0,NULL,NULL)==STATUS_NOT_IMPLEMENTED);
    assert(!pw_mutex_word_load(&without_access->word));
    assert(pw_mutex_word_try_acquire(&without_access->word,fixture_thread.ps5_mutex_token));
    assert(server_try_shared_mutex(8,1,NULL,NULL)==STATUS_SUCCESS);
    /* More than 64 distinct valid event handles retain exact negative slots. */
    for (unsigned i=0;i<80;i++) {
        unsigned h=1000+4*i; node(h,0,0);
        assert(server_try_shared_mutex(h,0,NULL,NULL)==STATUS_NOT_IMPLEMENTED);
    }
    cold_before=cold_calls; sections_before=sections;
    for (unsigned pass=0;pass<25;pass++) for (unsigned i=0;i<80;i++)
        assert(server_try_shared_mutex(1000+4*i,0,NULL,NULL)==STATUS_NOT_IMPLEMENTED);
    assert(cold_calls==cold_before && sections==sections_before);
    /* A valid cold retry must not create a lasting negative. */
    struct node *retry=node(1400,1,SYNCHRONIZE); retry_once=1;
    assert(server_try_shared_mutex(1400,0,NULL,NULL)==STATUS_NOT_IMPLEMENTED);
    assert(!__atomic_load_n(shared_mutex_slot(1400,0),__ATOMIC_ACQUIRE)); take_release(1400);
    assert(!pw_mutex_word_load(&retry->word));
    /* A second page is allocated only on cold fill; other slots stay intact. */
    unsigned high=4*(PW_MUTEX_CACHE_SLOTS+7)+4; node(high,1,SYNCHRONIZE); take_release(high);
    assert(page_count==1); cold_before=cold_calls; sections_before=sections; take_release(high);
    assert(cold_calls==cold_before && sections==sections_before);
    /* Readiness downgrade is a fallback with zero extra cold calls/locks. */
    __atomic_store_n(&ready,0,__ATOMIC_RELEASE); cold_before=cold_calls; sections_before=sections;
    assert(server_try_shared_mutex(4,0,NULL,NULL)==STATUS_NOT_IMPLEMENTED);
    assert(cold_calls==cold_before && sections==sections_before && !pw_mutex_word_load(&first->word));
    __atomic_store_n(&ready,1,__ATOMIC_RELEASE);
    /* Temporary ordinary SLOW mode can recover without a cache refill. */
    uint64_t snapshot;
    assert(pw_mutex_word_freeze(&first->word,&snapshot) && !snapshot);
    cold_before=cold_calls; sections_before=sections;
    assert(server_try_shared_mutex(4,0,NULL,NULL)==STATUS_NOT_IMPLEMENTED);
    assert(cold_calls==cold_before && sections==sections_before);
    pw_mutex_word_publish(&first->word,0); take_release(4);
    assert(cold_calls==cold_before && sections==sections_before);
    /* Complete ordinary close/invalidation then reuse for a different cell.
     * Retired storage is retained and never recycled by the fixture. */
    assert(!pthread_mutex_lock(&fd_cache_mutex));
    uint64_t old; pw_mutex_word_freeze(&first->word,&old); first->handle=0;
    server_clear_shared_mutex_slot(4); struct node *replacement=node(4,1,SYNCHRONIZE);
    assert(!pthread_mutex_unlock(&fd_cache_mutex)); take_release(4);
    assert(pw_mutex_word_load(&first->word)&PW_MUTEX_WORD_SLOW);
    assert(!pw_mutex_word_load(&replacement->word));
    /* Legal concurrent ownership on the real client/CAS path. Each live
     * thread needs its own token even when another filled the shared slot. */
    pthread_t threads[4];
    for (unsigned i=0;i<4;i++) assert(!pthread_create(&threads[i],NULL,worker,(void *)(uintptr_t)(10+i)));
    for (unsigned i=0;i<4;i++) assert(!pthread_join(threads[i],NULL));
    assert(protected_counter==12000 && !pw_mutex_word_load(&replacement->word));
    assert(global_error==7 && !current && !server_depth);
    for (unsigned i=0;i<256;i++) assert(!server_threads[i].refs);
    for (unsigned i=0;i<page_count;i++) free(pages[i]);
    printf("PASS: strict independent switches, 15 lookup gates, 24000 warm hits without cold calls/locks, 80 exact negatives, legal lifecycle, 12000 concurrent sections\n");
    return 0;
}
