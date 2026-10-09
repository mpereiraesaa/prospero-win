/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "wine/ps5/pw_d3d9_bridge_wire.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct fixture { unsigned char *memory; size_t bytes; struct pw_d3d9_channel client,service; };
static void init(struct fixture *f,uint32_t capacity)
{
    f->bytes=pw_d3d9_channel_bytes(capacity,capacity);assert(f->bytes);
    f->memory=calloc(1,f->bytes);assert(f->memory);
    assert(pw_d3d9_channel_init(f->memory,f->bytes,7,capacity,capacity)==PW_D3D9_OK);
    assert(pw_d3d9_channel_open(&f->client,f->memory,f->bytes,7,PW_D3D9_CLIENT)==PW_D3D9_OK);
    assert(pw_d3d9_channel_open(&f->service,f->memory,f->bytes,7,PW_D3D9_SERVICE)==PW_D3D9_OK);
}
static void hello(struct fixture *f)
{
    unsigned char scratch[128];struct pw_d3d9_message m={.opcode=PW_D3D9_HELLO},r;
    assert(pw_d3d9_channel_ready(&f->service)==PW_D3D9_INVALID);
    assert(pw_d3d9_channel_send(&f->client,&m,NULL)==PW_D3D9_OK);
    assert(pw_d3d9_channel_receive(&f->service,&r,scratch,sizeof(scratch))==PW_D3D9_OK);
    assert(r.sequence==1 && r.ticket==1);
    assert(pw_d3d9_channel_ready(&f->service)==PW_D3D9_OK);
    r.sequence=0;assert(pw_d3d9_channel_send(&f->service,&r,NULL)==PW_D3D9_OK);
    assert(pw_d3d9_channel_receive(&f->client,&r,scratch,sizeof(scratch))==PW_D3D9_OK);
}
static struct pw_d3d9_message call(uint32_t bytes)
{ return (struct pw_d3d9_message){.opcode=PW_D3D9_FACTORY_CALL,.device=9,.object=12,.generation=3,.payload_bytes=bytes}; }
static void roundtrip(struct fixture *f,unsigned value)
{
    unsigned char payload[35],scratch[128];struct pw_d3d9_message m=call(sizeof(payload)),r;
    const unsigned operations[]={PW_D3D9_FACTORY_CALL,PW_D3D9_DEVICE_CALL,PW_D3D9_RESOURCE_CALL,
        PW_D3D9_COMMAND_CALL,PW_D3D9_PROGRAM_CALL,PW_D3D9_TEXTURE_CALL,PW_D3D9_GETTER_CALL,PW_D3D9_OBJECT_GETTER_CALL,PW_D3D9_STATEBLOCK_CALL,PW_D3D9_PROGRAM_QUERY_CALL,PW_D3D9_UP_DRAW_CALL,PW_D3D9_QUERY_CALL,PW_D3D9_CURSOR_CALL,PW_D3D9_GAMMA_CALL,PW_D3D9_IMPLICIT_CALL,PW_D3D9_COMMAND_BATCH_CALL};
    m.opcode=operations[value % (sizeof(operations)/sizeof(operations[0]))];
    memset(payload,(int)value,sizeof(payload));
    assert(pw_d3d9_channel_send(&f->client,&m,payload)==PW_D3D9_OK);
    memset(payload,0,sizeof(payload));
    assert(pw_d3d9_channel_receive(&f->service,&r,scratch,63)==PW_D3D9_SMALL);
    assert(pw_d3d9_channel_receive(&f->service,&r,scratch,sizeof(scratch))==PW_D3D9_OK);
    for(unsigned i=0;i<35;i++)assert(scratch[64+i]==(unsigned char)value);
    r.sequence=0;r.result=(int32_t)0x8876086c;
    assert(pw_d3d9_channel_send(&f->service,&r,scratch+64)==PW_D3D9_OK);
    memset(scratch,0,sizeof(scratch));
    assert(pw_d3d9_channel_receive(&f->client,&r,scratch,sizeof(scratch))==PW_D3D9_OK);
    assert(r.result==(int32_t)0x8876086c && r.object==12 && r.generation==3 && r.opcode==m.opcode);
    for(unsigned i=0;i<35;i++)assert(scratch[64+i]==(unsigned char)value);
}
static void basic(void)
{
    struct fixture f;unsigned char scratch[128];struct pw_d3d9_message m,r;init(&f,128);
    assert(pw_d3d9_channel_open(&f.client,f.memory,f.bytes,8,PW_D3D9_CLIENT)==PW_D3D9_INVALID);
    assert(pw_d3d9_channel_receive(&f.client,&r,scratch,sizeof(scratch))==PW_D3D9_EMPTY);
    hello(&f);
    m=(struct pw_d3d9_message){.opcode=PW_D3D9_STOP};
    assert(pw_d3d9_channel_send(&f.client,&m,NULL)==PW_D3D9_INVALID);
    m.opcode=34;assert(pw_d3d9_channel_send(&f.client,&m,NULL)==PW_D3D9_INVALID);
    for(unsigned i=0;i<500;i++)roundtrip(&f,i);
    /* The positions are unsigned modular counters; simulate the 4GiB boundary. */
    for(unsigned i=0;i<4;i++)atomic_store((_Atomic uint32_t *)(f.memory+48+i*4),UINT32_MAX-63);
    roundtrip(&f,91);
    m=call(65);assert(pw_d3d9_channel_send(&f.client,&m,scratch)==PW_D3D9_INVALID);
    m=call(0);assert(pw_d3d9_channel_send(&f.client,&m,NULL)==PW_D3D9_OK);
    assert(pw_d3d9_channel_stop(&f.client)==PW_D3D9_OK);
    assert(pw_d3d9_channel_stopped(&f.service)==PW_D3D9_FULL);
    m=(struct pw_d3d9_message){.opcode=PW_D3D9_STOP};
    assert(pw_d3d9_channel_send(&f.client,&m,NULL)==PW_D3D9_OK);
    assert(pw_d3d9_channel_send(&f.client,&m,NULL)==PW_D3D9_INVALID);
    for(unsigned i=0;i<2;i++){
        assert(pw_d3d9_channel_receive(&f.service,&r,scratch,sizeof(scratch))==PW_D3D9_OK);
        r.sequence=0;assert(pw_d3d9_channel_send(&f.service,&r,NULL)==PW_D3D9_OK);
    }
    assert(pw_d3d9_channel_stopped(&f.service)==PW_D3D9_OK);
    for(unsigned i=0;i<2;i++)assert(pw_d3d9_channel_receive(&f.client,&r,scratch,sizeof(scratch))==PW_D3D9_OK);
    assert(pw_d3d9_channel_receive(&f.client,&r,scratch,sizeof(scratch))==PW_D3D9_CLOSED);
    assert(pw_d3d9_channel_send(&f.client,&m,NULL)==PW_D3D9_CLOSED);free(f.memory);
}
static void early_stop(void)
{
    struct fixture f;unsigned char scratch[128];struct pw_d3d9_message m=call(0),r;
    init(&f,128);hello(&f);
    assert(pw_d3d9_channel_send(&f.client,&m,NULL)==PW_D3D9_OK);
    /* Rewrite the queued opcode into an otherwise valid premature STOP. */
    f.memory[128+64+12]=PW_D3D9_STOP;
    assert(pw_d3d9_channel_receive(&f.service,&r,scratch,sizeof(scratch))==PW_D3D9_INVALID);
    assert(pw_d3d9_channel_error(&f.client));free(f.memory);
}
static void full_and_cancel(void)
{
    struct fixture f;struct pw_d3d9_message m=call(0),r;unsigned char scratch[128];init(&f,4096);hello(&f);
    for(unsigned i=0;i<PW_D3D9_WIRE_PENDING;i++)assert(pw_d3d9_channel_send(&f.client,&m,NULL)==PW_D3D9_OK);
    assert(pw_d3d9_channel_send(&f.client,&m,NULL)==PW_D3D9_FULL);
    pw_d3d9_channel_cancel(&f.service,123);pw_d3d9_channel_cancel(&f.client,456);
    assert(pw_d3d9_channel_error(&f.client)==123);
    assert(pw_d3d9_channel_receive(&f.service,&r,scratch,sizeof(scratch))==PW_D3D9_CLOSED);
    assert(pw_d3d9_channel_send(&f.client,&m,NULL)==PW_D3D9_CLOSED);free(f.memory);
    init(&f,128);hello(&f);m=call(0);
    assert(pw_d3d9_channel_send(&f.client,&m,NULL)==PW_D3D9_OK);
    assert(pw_d3d9_channel_send(&f.client,&m,NULL)==PW_D3D9_OK);
    assert(pw_d3d9_channel_send(&f.client,&m,NULL)==PW_D3D9_FULL);free(f.memory);
}
static void corruption(void)
{
    const unsigned fields[]={0,4,8,12,16,20,36,40,48,60,65};
    for(unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);i++) {
        struct fixture f;unsigned char scratch[128],payload=1;struct pw_d3d9_message m=call(1),r;
        init(&f,128);hello(&f);assert(pw_d3d9_channel_send(&f.client,&m,&payload)==PW_D3D9_OK);
        /* HELLO consumed 64 bytes, so the next frame begins halfway in ring. */
        f.memory[128+((64+fields[i])&127)]^=0x80;
        assert(pw_d3d9_channel_receive(&f.service,&r,scratch,sizeof(scratch))==PW_D3D9_INVALID);
        assert(pw_d3d9_channel_error(&f.client));free(f.memory);
    }
    /* Replies must match the pending target generation, not just ticket. */
    struct fixture f;unsigned char scratch[128];struct pw_d3d9_message m=call(0),r;init(&f,128);hello(&f);
    assert(pw_d3d9_channel_send(&f.client,&m,NULL)==PW_D3D9_OK);
    assert(pw_d3d9_channel_receive(&f.service,&r,scratch,sizeof(scratch))==PW_D3D9_OK);
    r.sequence=0;r.generation++;assert(pw_d3d9_channel_send(&f.service,&r,NULL)==PW_D3D9_INVALID);
    r.generation--;assert(pw_d3d9_channel_send(&f.service,&r,NULL)==PW_D3D9_OK);
    f.memory[128+128+64+32]^=1;
    assert(pw_d3d9_channel_receive(&f.client,&r,scratch,sizeof(scratch))==PW_D3D9_INVALID);free(f.memory);
}
static uint32_t word(const void *p) { uint32_t value;memcpy(&value,p,sizeof(value));return value; }
static void *service(void *arg)
{
    struct fixture *f=arg;unsigned char scratch[128];struct pw_d3d9_message m;unsigned count=0;
    while(count<20000) {
        int rc=pw_d3d9_channel_receive(&f->service,&m,scratch,sizeof(scratch));
        if(rc==PW_D3D9_EMPTY){sched_yield();continue;}assert(rc==PW_D3D9_OK);
        assert(m.payload_bytes==4 && word(scratch+64)==count);
        m.sequence=0;
        do{rc=pw_d3d9_channel_send(&f->service,&m,scratch+64);if(rc==PW_D3D9_FULL)sched_yield();}while(rc==PW_D3D9_FULL);
        assert(rc==PW_D3D9_OK);count++;
    }
    return NULL;
}
static void concurrent(void)
{
    struct fixture f;pthread_t thread;unsigned sent=0,received=0;unsigned char scratch[128];struct pw_d3d9_message m=call(4),r;
    init(&f,1024);hello(&f);assert(!pthread_create(&thread,NULL,service,&f));
    while(received<20000) {
        if(sent<20000){int rc=pw_d3d9_channel_send(&f.client,&m,&sent);assert(rc==PW_D3D9_OK||rc==PW_D3D9_FULL);if(rc==PW_D3D9_OK)sent++;}
        int rc=pw_d3d9_channel_receive(&f.client,&r,scratch,sizeof(scratch));
        assert(rc==PW_D3D9_OK||rc==PW_D3D9_EMPTY);if(rc==PW_D3D9_OK){assert(word(scratch+64)==received);received++;}else sched_yield();
    }
    assert(!pthread_join(thread,NULL));assert(!f.client.pending_count && !f.service.pending_count);free(f.memory);
}
/* Conditional wakeups with no spinning: every empty receive takes the sleep
 * path, so a lost wakeup shows up as a timed-out wait. Semaphores stand in for
 * the Win32 auto-reset events; stale posts only cause an extra empty pass. */
struct sleeper { struct fixture *f; sem_t wake[2]; atomic_uint signals[2]; };
static int sleep_receive(struct sleeper *s,struct pw_d3d9_channel *c,struct pw_d3d9_message *m,unsigned char *scratch,size_t bytes)
{
    for(;;){
        int rc=pw_d3d9_channel_receive(c,m,scratch,bytes);
        if(rc!=PW_D3D9_EMPTY)return rc;
        pw_d3d9_channel_sleep(c,1);
        rc=pw_d3d9_channel_receive(c,m,scratch,bytes);
        if(rc!=PW_D3D9_EMPTY){pw_d3d9_channel_sleep(c,0);return rc;}
        struct timespec t;assert(!clock_gettime(CLOCK_REALTIME,&t));t.tv_sec+=5;
        int waited=sem_timedwait(&s->wake[c->role],&t);
        pw_d3d9_channel_sleep(c,0);assert(!waited);
    }
}
static void sleep_send(struct sleeper *s,struct pw_d3d9_channel *c,const struct pw_d3d9_message *m,const void *payload)
{
    assert(pw_d3d9_channel_send(c,m,payload)==PW_D3D9_OK);
    if(pw_d3d9_channel_peer_sleeping(c)){atomic_fetch_add(&s->signals[c->role],1);assert(!sem_post(&s->wake[1-c->role]));}
}
static void *sleepy_service(void *arg)
{
    struct sleeper *s=arg;unsigned char scratch[128];struct pw_d3d9_message m;
    for(unsigned count=0;count<20000;count++){
        assert(sleep_receive(s,&s->f->service,&m,scratch,sizeof(scratch))==PW_D3D9_OK);
        assert(word(scratch+64)==count);m.sequence=0;sleep_send(s,&s->f->service,&m,scratch+64);
    }
    return NULL;
}
static void conditional_wakeups(void)
{
    struct fixture f;struct sleeper s={.f=&f};pthread_t thread;unsigned char scratch[128];struct pw_d3d9_message m=call(4),r;
    init(&f,1024);hello(&f);
    struct shared_probe { unsigned char head[80]; uint32_t sleeping[2]; } *probe=(void *)f.memory;
    assert(!probe->sleeping[0]&&!probe->sleeping[1]&&!pw_d3d9_channel_peer_sleeping(&f.client));
    pw_d3d9_channel_sleep(&f.service,1);assert(probe->sleeping[1]==1&&pw_d3d9_channel_peer_sleeping(&f.client));
    assert(!pw_d3d9_channel_peer_sleeping(&f.service));pw_d3d9_channel_sleep(&f.service,0);assert(!probe->sleeping[1]);
    for(unsigned i=0;i<2;i++){assert(!sem_init(&s.wake[i],0,0));atomic_init(&s.signals[i],0);}
    assert(!pthread_create(&thread,NULL,sleepy_service,&s));
    for(unsigned count=0;count<20000;count++){
        sleep_send(&s,&f.client,&m,&count);
        assert(sleep_receive(&s,&f.client,&r,scratch,sizeof(scratch))==PW_D3D9_OK&&word(scratch+64)==count);
    }
    assert(!pthread_join(thread,NULL));
    /* Both sides blocked at least once; otherwise the race was not exercised. */
    assert(atomic_load(&s.signals[0])&&atomic_load(&s.signals[1]));
    for(unsigned i=0;i<2;i++)sem_destroy(&s.wake[i]);
    free(f.memory);
    /* A nonzero flag in a fresh section is rejected like other stale state. */
    init(&f,128);struct fixture g={.bytes=f.bytes};
    g.memory=calloc(1,g.bytes);assert(g.memory);
    assert(pw_d3d9_channel_init(g.memory,g.bytes,7,128,128)==PW_D3D9_OK);
    ((struct shared_probe *)g.memory)->sleeping[0]=1;
    assert(pw_d3d9_channel_open(&g.client,g.memory,g.bytes,7,PW_D3D9_CLIENT)==PW_D3D9_INVALID);
    free(g.memory);free(f.memory);
}
int main(void)
{
    assert(!pw_d3d9_channel_bytes(127,128));assert(!pw_d3d9_channel_bytes(PW_D3D9_WIRE_MAX_RING*2,128));
    assert(pw_d3d9_wire_slice(64,16,16,48,8)==PW_D3D9_OK);
    assert(pw_d3d9_wire_slice(64,16,0,0,8)==PW_D3D9_OK);
    assert(pw_d3d9_wire_slice(64,16,8,8,8)==PW_D3D9_INVALID);
    assert(pw_d3d9_wire_slice(64,16,16,UINT32_MAX,8)==PW_D3D9_INVALID);
    assert(pw_d3d9_wire_slice(64,16,16,0,8)==PW_D3D9_INVALID);
    basic();early_stop();full_and_cancel();corruption();concurrent();conditional_wakeups();
    puts("D3D9 transport: framing, ownership, wrap, tickets, generations, backpressure, cancellation, stop, 20000 concurrent replies and 20000 conditional-wakeup round trips passed");return 0;
}
