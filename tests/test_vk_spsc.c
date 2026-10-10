/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <assert.h>
#ifdef _WIN32
#include <windows.h>
typedef HANDLE test_thread;
#define THREAD_RESULT DWORD WINAPI
#define yield_thread() SwitchToThread()
#else
#include <pthread.h>
#include <sched.h>
typedef pthread_t test_thread;
#define THREAD_RESULT void *
#define yield_thread() sched_yield()
#endif
#include <stdio.h>
#include <string.h>
#include "wine/ps5/pw_vk_spsc.h"

static void basic(void)
{
    struct pw_vk_spsc_sequence seq;
    struct pw_vk_spsc s;
    unsigned char arena[64], out[80], payload[17];
    size_t n; unsigned i;
    pw_vk_spsc_sequence_init(&seq);
    assert(pw_vk_spsc_init(&s, arena, sizeof(arena)) == PW_VK_STREAM_OK);
    memset(payload, 0x71, sizeof(payload));
    for (i = 1; i < 100; ++i) {
        assert(pw_vk_spsc_append(&seq, &s, 3, payload, sizeof(payload)) == PW_VK_STREAM_OK);
        assert(pw_vk_spsc_marker(&seq) == i);
        assert(pw_vk_spsc_append(&seq, &s, 3, payload, sizeof(payload)) == PW_VK_STREAM_FULL);
        assert(pw_vk_spsc_marker(&seq) == i);
        assert(pw_vk_spsc_take(&s, i, out, 16, &n) == PW_VK_STREAM_FULL);
        assert(pw_vk_spsc_take(&s, i, out, sizeof(out), &n) == PW_VK_STREAM_OK);
        assert(n == 56 && !memcmp(out + PW_VK_STREAM_HEADER, payload, sizeof(payload)));
        assert(pw_vk_spsc_take(&s, i + 1, out, sizeof(out), &n) == PW_VK_STREAM_PENDING);
    }
    assert(pw_vk_spsc_append(&seq, &s, 3, arena, 1) == PW_VK_STREAM_INVALID);
    atomic_store(&seq.next, UINT64_MAX);
    assert(pw_vk_spsc_append(&seq, &s, 3, payload, 1) == PW_VK_STREAM_SEQUENCE_EXHAUSTED);
}
static void errors(void)
{
    struct pw_vk_spsc_sequence seq;struct pw_vk_spsc s;
    unsigned char arena[128],out[128];size_t n;uint64_t read;
    pw_vk_spsc_sequence_init(&seq);
    assert(pw_vk_spsc_init(&s,arena,127)==PW_VK_STREAM_INVALID);
    assert(pw_vk_spsc_init(&s,arena,16)==PW_VK_STREAM_INVALID);
    assert(pw_vk_spsc_init(&s,arena,sizeof(arena))==0);
    assert(pw_vk_spsc_append(&seq,&s,0,NULL,0)==PW_VK_STREAM_INVALID);
    assert(pw_vk_spsc_append(&seq,&s,1,NULL,1)==PW_VK_STREAM_INVALID);
    assert(pw_vk_spsc_marker(&seq)==0);
    assert(pw_vk_spsc_append(&seq,&s,1,"x",1)==0);
    read=atomic_load(&s.read);
    assert(pw_vk_spsc_take(&s,1,arena,sizeof(arena),&n)==PW_VK_STREAM_INVALID);
    assert(atomic_load(&s.read)==read);
    arena[39]=1; /* Invalid framing padding cannot release storage. */
    assert(pw_vk_spsc_take(&s,1,out,sizeof(out),&n)==PW_VK_STREAM_INVALID);
    assert(atomic_load(&s.read)==read);
    arena[39]=0;
    assert(pw_vk_spsc_take(&s,1,out,sizeof(out),&n)==0);
    /* Ring counters stop before wrap even if sequence capacity remains. */
    atomic_store(&s.read,UINT64_MAX-15);atomic_store(&s.write,UINT64_MAX-15);
    assert(pw_vk_spsc_append(&seq,&s,1,NULL,0)==PW_VK_STREAM_SEQUENCE_EXHAUSTED);
    assert(pw_vk_spsc_marker(&seq)==1);
}
static void gap(void)
{
    struct pw_vk_spsc_sequence seq;
    struct pw_vk_spsc s;
    unsigned char arena[128], out[128]; size_t n;
    pw_vk_spsc_sequence_init(&seq);assert(pw_vk_spsc_init(&s, arena, sizeof(arena)) == 0);
    /* Model another producer preempted after reserving ticket 1. */
    assert(atomic_fetch_add(&seq.next, 1) == 1);
    assert(pw_vk_spsc_append(&seq, &s, 1, "second", 7) == 0);
    assert(pw_vk_spsc_marker(&seq) == 2);
    assert(pw_vk_spsc_take(&s, 1, out, sizeof(out), &n) == PW_VK_STREAM_PENDING);
    assert(atomic_load(&s.read) == 0);
    assert(pw_vk_spsc_append(&seq, &s, 1, "third", 6) == 0);
    assert(pw_vk_spsc_marker(&seq) == 3); /* Later producer still progresses. */
    /* The adapter would consume ticket 1 from its other ring before this. */
    assert(pw_vk_spsc_take(&s, 2, out, sizeof(out), &n) == 0);
    assert(pw_vk_spsc_take(&s, 3, out, sizeof(out), &n) == 0);
}
#define PRODUCERS 3
#define RECORDS 10000
struct fixture {
    struct pw_vk_spsc_sequence seq;
    struct pw_vk_spsc stream[PRODUCERS];
    unsigned char arena[PRODUCERS][512];
};
struct producer_arg { struct fixture *f; unsigned id; };
static THREAD_RESULT produce(void *opaque)
{
    struct producer_arg *a=opaque; unsigned i;
    for(i=0;i<RECORDS;++i) {
        unsigned payload[4]={a->id,i,0x12345678,~i};int status;
        do {
            status=pw_vk_spsc_append(&a->f->seq,&a->f->stream[a->id],7,payload,sizeof(payload));
            if(status==PW_VK_STREAM_FULL)yield_thread();
        } while(status==PW_VK_STREAM_FULL);
        assert(status==0);memset(payload,0,sizeof(payload));
    }
    return 0;
}
static void start_thread(test_thread *thread,struct producer_arg *arg)
{
#ifdef _WIN32
    *thread=CreateThread(NULL,0,produce,arg,0,NULL);assert(*thread);
#else
    assert(!pthread_create(thread,NULL,produce,arg));
#endif
}
static void join_thread(test_thread thread)
{
#ifdef _WIN32
    assert(WaitForSingleObject(thread,INFINITE)==WAIT_OBJECT_0);assert(CloseHandle(thread));
#else
    assert(!pthread_join(thread,NULL));
#endif
}
static void concurrent(void)
{
    struct fixture f;struct producer_arg args[PRODUCERS];test_thread threads[PRODUCERS];
    unsigned next[PRODUCERS]={0},i;uint64_t ticket=1;unsigned char out[80];size_t n;
    pw_vk_spsc_sequence_init(&f.seq);
    for(i=0;i<PRODUCERS;++i){assert(pw_vk_spsc_init(&f.stream[i],f.arena[i],sizeof(f.arena[i]))==0);args[i]=(struct producer_arg){&f,i};start_thread(&threads[i],&args[i]);}
    while(ticket<=PRODUCERS*RECORDS) {
        int found=0;
        for(i=0;i<PRODUCERS;++i){
            int status=pw_vk_spsc_take(&f.stream[i],ticket,out,sizeof(out),&n);
            if(status==PW_VK_STREAM_PENDING)continue;
            assert(status==0);assert(n==48);
            {unsigned payload[4];memcpy(payload,out+PW_VK_STREAM_HEADER,sizeof(payload));assert(payload[0]==i&&payload[1]==next[i]++&&payload[2]==0x12345678&&payload[3]==~payload[1]);}
            ++ticket;found=1;break;
        }
        if(!found)yield_thread();
    }
    for(i=0;i<PRODUCERS;++i){join_thread(threads[i]);assert(next[i]==RECORDS);}
    assert(pw_vk_spsc_marker(&f.seq)==PRODUCERS*RECORDS);
}
int main(void){basic();errors();gap();concurrent();puts("PASS SPSC ownership, wrap, capacity, pending reservation and concurrent global order");return 0;}
