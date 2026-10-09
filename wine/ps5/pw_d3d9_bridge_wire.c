/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_bridge_wire.h"
#include <stdatomic.h>
#include <string.h>
#define MAGIC 0x39445750u
#define FAILED 0x80000000u
#define MALFORMED 1u
struct shared {
    uint32_t magic, version, header, bytes, epoch, reserved[3];
    uint32_t offset[2], capacity[2];
    _Atomic uint32_t read[2], write[2];
    _Atomic uint32_t state;
    uint32_t padding[3];
    /* Indexed by role: set only while that endpoint is about to block or is
     * blocked on its wake event. Senders signal only a sleeping peer. */
    _Atomic uint32_t sleeping[2];
};
/* Fixed offsets, identical in PE32 and PE64; no pointer/size_t/COM layout. */
_Static_assert(sizeof(struct shared) == 88, "shared wire size");
_Static_assert(offsetof(struct shared, sleeping) == 80, "shared sleeping offset");
_Static_assert(offsetof(struct shared, read) == 48, "shared read offset");
_Static_assert(offsetof(struct shared, state) == 64, "shared state offset");
_Static_assert(sizeof(_Atomic uint32_t) == 4, "shared atomics");
static int power2(uint32_t n) { return n >= 128 && n <= PW_D3D9_WIRE_MAX_RING && !(n & (n - 1)); }
static int range(const void *p, size_t n) { return p && n <= UINTPTR_MAX - (uintptr_t)p; }
static int overlap(const void *a,size_t an,const void *b,size_t bn)
{ return an && bn && (uintptr_t)a < (uintptr_t)b + bn && (uintptr_t)b < (uintptr_t)a + an; }
static int little(void) { const uint32_t one=1; return *(const unsigned char *)&one; }
static struct shared *shared(const struct pw_d3d9_channel *c) { return (struct shared *)c->memory; }
static void put32(unsigned char *p,uint32_t n) { unsigned i;for(i=0;i<4;i++)p[i]=(unsigned char)(n>>(i*8)); }
static uint32_t get32(const unsigned char *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void put64(unsigned char *p,uint64_t n) { put32(p,(uint32_t)n);put32(p+4,(uint32_t)(n>>32)); }
static uint64_t get64(const unsigned char *p) { return get32(p)|(uint64_t)get32(p+4)<<32; }
static int opcode(uint32_t op) { return op==PW_D3D9_HELLO || op==PW_D3D9_STOP || op==PW_D3D9_CREATE9 || op==PW_D3D9_FACTORY_CALL || op==PW_D3D9_RELEASE || op==PW_D3D9_DEVICE_CALL || op==PW_D3D9_RESOURCE_CALL || op==PW_D3D9_COMMAND_CALL || op==PW_D3D9_PROGRAM_CALL || op==PW_D3D9_TEXTURE_CALL || op==PW_D3D9_GETTER_CALL || op==PW_D3D9_OBJECT_GETTER_CALL || op==PW_D3D9_STATEBLOCK_CALL || op==PW_D3D9_PROGRAM_QUERY_CALL || op==PW_D3D9_UP_DRAW_CALL || op==PW_D3D9_QUERY_CALL || op==PW_D3D9_CURSOR_CALL || op==PW_D3D9_GAMMA_CALL || op==PW_D3D9_IMPLICIT_CALL || op==PW_D3D9_COMMAND_BATCH_CALL; }
static int same(const struct pw_d3d9_message *a,const struct pw_d3d9_message *b)
{ return a->opcode==b->opcode && a->device==b->device && a->object==b->object && a->generation==b->generation; }
static void copy(struct pw_d3d9_channel *c,unsigned ring,uint32_t at,void *data,size_t n,int input)
{
    size_t pos=at&(c->capacity[ring]-1),first=c->capacity[ring]-pos;
    unsigned char *arena=c->memory+c->offset[ring];
    if(first>n)first=n;
    if(input) { memcpy(arena+pos,data,first);if(n>first)memcpy(arena,(unsigned char *)data+first,n-first); }
    else { memcpy(data,arena+pos,first);if(n>first)memcpy((unsigned char *)data+first,arena,n-first); }
}
size_t pw_d3d9_channel_bytes(uint32_t request,uint32_t reply)
{ return power2(request)&&power2(reply) ? 128u+(size_t)request+reply : 0; }
int pw_d3d9_channel_init(void *memory,size_t bytes,uint32_t epoch,uint32_t request,uint32_t reply)
{
    struct shared *s=memory;
    size_t expected=pw_d3d9_channel_bytes(request,reply);
    if(!little() || !range(memory,bytes) || ((uintptr_t)memory&7) || !epoch || !expected || bytes!=expected)return PW_D3D9_INVALID;
    memset(memory,0,bytes);s->magic=MAGIC;s->version=PW_D3D9_WIRE_VERSION;s->header=128;s->bytes=(uint32_t)bytes;s->epoch=epoch;
    s->offset[0]=128;s->offset[1]=128+request;s->capacity[0]=request;s->capacity[1]=reply;
    atomic_init(&s->read[0],0);atomic_init(&s->read[1],0);atomic_init(&s->write[0],0);atomic_init(&s->write[1],0);atomic_init(&s->state,PW_D3D9_STARTING);
    atomic_init(&s->sleeping[0],0);atomic_init(&s->sleeping[1],0);
    return atomic_is_lock_free(&s->state)&&atomic_is_lock_free(&s->read[0]) ? PW_D3D9_OK : PW_D3D9_INVALID;
}
int pw_d3d9_channel_open(struct pw_d3d9_channel *c,void *memory,size_t bytes,uint32_t epoch,enum pw_d3d9_wire_role role)
{
    struct shared *s=memory;
    unsigned i;
    if(!little() || !c || !range(memory,bytes) || bytes<128 || ((uintptr_t)memory&7) ||
       overlap(c,sizeof(*c),memory,bytes) || !epoch || (role!=PW_D3D9_CLIENT && role!=PW_D3D9_SERVICE))return PW_D3D9_INVALID;
    if(s->magic!=MAGIC || s->version!=PW_D3D9_WIRE_VERSION || s->header!=128 || s->bytes!=bytes || s->epoch!=epoch ||
       s->offset[0]!=128 || !power2(s->capacity[0]) || !power2(s->capacity[1]) || s->offset[1]!=128+s->capacity[0] ||
       bytes!=pw_d3d9_channel_bytes(s->capacity[0],s->capacity[1]) || !atomic_is_lock_free(&s->state))return PW_D3D9_INVALID;
    for(i=0;i<3;i++)if(s->reserved[i] || s->padding[i])return PW_D3D9_INVALID;
    for(i=sizeof(*s);i<128;i++)if(((unsigned char *)memory)[i])return PW_D3D9_INVALID;
    if(atomic_load_explicit(&s->state,memory_order_acquire)!=PW_D3D9_STARTING)return PW_D3D9_INVALID;
    for(i=0;i<2;i++)if(atomic_load(&s->read[i]) || atomic_load(&s->write[i]) || atomic_load(&s->sleeping[i]))return PW_D3D9_INVALID;
    memset(c,0,sizeof(*c));c->memory=memory;c->bytes=bytes;c->epoch=epoch;c->role=role;c->next_send=c->next_receive=1;
    for(i=0;i<2;i++){c->offset[i]=s->offset[i];c->capacity[i]=s->capacity[i];}
    return PW_D3D9_OK;
}
uint32_t pw_d3d9_channel_state(const struct pw_d3d9_channel *c)
{ return atomic_load_explicit(&shared(c)->state,memory_order_acquire); }
uint32_t pw_d3d9_channel_error(const struct pw_d3d9_channel *c)
{ uint32_t state=pw_d3d9_channel_state(c);return state&FAILED ? state&~FAILED : 0; }
void pw_d3d9_channel_cancel(struct pw_d3d9_channel *c,uint32_t error)
{
    uint32_t state=pw_d3d9_channel_state(c);
    if(!error || (error&FAILED))error=MALFORMED;
    while(!(state&FAILED) && !atomic_compare_exchange_weak_explicit(&shared(c)->state,&state,FAILED|error,memory_order_acq_rel,memory_order_acquire)) {}
}
static int transition(struct pw_d3d9_channel *c,uint32_t from,uint32_t to,unsigned role)
{
    if(c->role!=role)return PW_D3D9_INVALID;
    return atomic_compare_exchange_strong_explicit(&shared(c)->state,&from,to,memory_order_acq_rel,memory_order_acquire) ? PW_D3D9_OK : PW_D3D9_CLOSED;
}
int pw_d3d9_channel_ready(struct pw_d3d9_channel *c)
{
    if(c->next_receive!=2 || c->next_send!=1 || c->pending_count!=1 || c->pending[0].opcode!=PW_D3D9_HELLO)return PW_D3D9_INVALID;
    return transition(c,PW_D3D9_STARTING,PW_D3D9_READY,PW_D3D9_SERVICE);
}
int pw_d3d9_channel_stop(struct pw_d3d9_channel *c)
{ return transition(c,PW_D3D9_READY,PW_D3D9_STOPPING,PW_D3D9_CLIENT); }
int pw_d3d9_channel_stopped(struct pw_d3d9_channel *c)
{
    struct shared *s=shared(c);
    if(c->next_send<3 || c->pending[(c->next_send-2)%PW_D3D9_WIRE_PENDING].opcode!=PW_D3D9_STOP || c->pending_count || atomic_load_explicit(&s->read[0],memory_order_acquire)!=atomic_load_explicit(&s->write[0],memory_order_acquire))return PW_D3D9_FULL;
    return transition(c,PW_D3D9_STOPPING,PW_D3D9_STOPPED,PW_D3D9_SERVICE);
}
static int bad(struct pw_d3d9_channel *c) { pw_d3d9_channel_cancel(c,MALFORMED);return PW_D3D9_INVALID; }
/* Dekker pairing with pw_d3d9_channel_peer_sleeping: the sleeper's flag store
 * and the sender's write-index store are each followed by a full fence before
 * the other side's load, so at least one side observes the other. */
void pw_d3d9_channel_sleep(struct pw_d3d9_channel *c,int sleeping)
{
    atomic_store_explicit(&shared(c)->sleeping[c->role],sleeping?1u:0u,memory_order_seq_cst);
    atomic_thread_fence(memory_order_seq_cst);
}
int pw_d3d9_channel_peer_sleeping(const struct pw_d3d9_channel *c)
{
    atomic_thread_fence(memory_order_seq_cst);
    return atomic_load_explicit(&shared(c)->sleeping[1-c->role],memory_order_seq_cst)!=0;
}
int pw_d3d9_channel_send(struct pw_d3d9_channel *c,const struct pw_d3d9_message *m,const void *payload)
{
    unsigned char header[PW_D3D9_WIRE_HEADER]={0},padding[8]={0};
    struct shared *s=shared(c);
    uint32_t state=pw_d3d9_channel_state(c),read,write,total,ring=c->role;
    uint64_t ticket;
    if((state&FAILED)||state==PW_D3D9_STOPPED)return PW_D3D9_CLOSED;
    if(!m || overlap(m,sizeof(*m),c->memory,c->bytes) || overlap(m,sizeof(*m),c,sizeof(*c)) || !opcode(m->opcode) || m->sequence || (!!m->object != !!m->generation) ||
       (m->payload_bytes && !range(payload,m->payload_bytes)) || overlap(payload,m->payload_bytes,c->memory,c->bytes) ||
       m->payload_bytes>c->capacity[ring]-PW_D3D9_WIRE_HEADER)return PW_D3D9_INVALID;
    if(m->opcode==PW_D3D9_STOP && state!=PW_D3D9_STOPPING)return PW_D3D9_INVALID;
    if(c->role==PW_D3D9_CLIENT && c->next_send>1 &&
       c->pending[(c->next_send-2)%PW_D3D9_WIRE_PENDING].opcode==PW_D3D9_STOP)return PW_D3D9_INVALID;
    if((state==PW_D3D9_STARTING && m->opcode!=PW_D3D9_HELLO) ||
       (state==PW_D3D9_STOPPING && c->role==PW_D3D9_CLIENT && m->opcode!=PW_D3D9_STOP))return PW_D3D9_CLOSED;
    if((c->next_send==1)!=(m->opcode==PW_D3D9_HELLO) ||
       (c->role==PW_D3D9_CLIENT && m->opcode!=PW_D3D9_HELLO && c->next_receive==1) ||
       (c->role==PW_D3D9_SERVICE && m->opcode==PW_D3D9_HELLO && state!=PW_D3D9_READY))return PW_D3D9_INVALID;
    if(c->next_send==UINT64_MAX)return bad(c);
    if(c->role==PW_D3D9_CLIENT) {
        if(m->ticket || m->result)return PW_D3D9_INVALID;
        if(c->pending_count==PW_D3D9_WIRE_PENDING)return PW_D3D9_FULL;
        ticket=c->next_send;
    } else {
        if(!c->pending_count || m->ticket!=c->next_send || !same(m,&c->pending[(c->next_send-1)%PW_D3D9_WIRE_PENDING]))return PW_D3D9_INVALID;
        ticket=m->ticket;
    }
    total=(PW_D3D9_WIRE_HEADER+m->payload_bytes+7)&~7u;
    write=atomic_load_explicit(&s->write[ring],memory_order_relaxed);read=atomic_load_explicit(&s->read[ring],memory_order_acquire);
    if((uint32_t)(write-read)>c->capacity[ring])return bad(c);
    if(total>c->capacity[ring]-(uint32_t)(write-read))return PW_D3D9_FULL;
    put32(header,MAGIC);put32(header+4,PW_D3D9_WIRE_VERSION);put32(header+8,total);put32(header+12,m->opcode);
    put32(header+16,m->payload_bytes);put32(header+20,c->role);put32(header+24,m->device);put32(header+28,m->object);
    put32(header+32,m->generation);put32(header+36,c->epoch);put64(header+40,c->next_send);put64(header+48,ticket);put32(header+56,(uint32_t)m->result);
    copy(c,ring,write,header,sizeof(header),1);
    if(m->payload_bytes)copy(c,ring,write+PW_D3D9_WIRE_HEADER,(void *)payload,m->payload_bytes,1);
    if(total>PW_D3D9_WIRE_HEADER+m->payload_bytes)copy(c,ring,write+PW_D3D9_WIRE_HEADER+m->payload_bytes,padding,total-PW_D3D9_WIRE_HEADER-m->payload_bytes,1);
    if(c->role==PW_D3D9_CLIENT){c->pending[(c->next_send-1)%PW_D3D9_WIRE_PENDING]=*m;c->pending_count++;}else c->pending_count--;
    c->next_send++;atomic_store_explicit(&s->write[ring],write+total,memory_order_release);return PW_D3D9_OK;
}
int pw_d3d9_channel_receive(struct pw_d3d9_channel *c,struct pw_d3d9_message *m,void *scratch,size_t bytes)
{
    struct shared *s=shared(c);struct pw_d3d9_message record;
    unsigned char header[PW_D3D9_WIRE_HEADER],*wire=scratch;
    uint32_t ring=1-c->role,read,write,total,payload,i,state=pw_d3d9_channel_state(c);
    if((state&FAILED) || (state==PW_D3D9_STOPPED && c->role==PW_D3D9_SERVICE))return PW_D3D9_CLOSED;
    if(!m || !range(scratch,bytes) || overlap(scratch,bytes,c->memory,c->bytes) || overlap(m,sizeof(*m),c->memory,c->bytes) ||
       overlap(m,sizeof(*m),scratch,bytes) || overlap(c,sizeof(*c),scratch,bytes) || overlap(c,sizeof(*c),m,sizeof(*m)))return PW_D3D9_INVALID;
    read=atomic_load_explicit(&s->read[ring],memory_order_relaxed);write=atomic_load_explicit(&s->write[ring],memory_order_acquire);
    if((uint32_t)(write-read)>c->capacity[ring])return bad(c);
    if(read==write)return state==PW_D3D9_STOPPED ? PW_D3D9_CLOSED : PW_D3D9_EMPTY;
    if((uint32_t)(write-read)<PW_D3D9_WIRE_HEADER)return bad(c);
    copy(c,ring,read,header,sizeof(header),0);total=get32(header+8);payload=get32(header+16);
    if(get32(header)!=MAGIC || get32(header+4)!=PW_D3D9_WIRE_VERSION || total<PW_D3D9_WIRE_HEADER || (total&7) || total>(uint32_t)(write-read) ||
       payload>total-PW_D3D9_WIRE_HEADER || total-PW_D3D9_WIRE_HEADER-payload>7 || get32(header+20)!=ring || get32(header+36)!=c->epoch || get32(header+60))return bad(c);
    record=(struct pw_d3d9_message){get32(header+12),get32(header+24),get32(header+28),get32(header+32),get64(header+40),get64(header+48),(int32_t)get32(header+56),payload};
    if((record.sequence==1)!=(record.opcode==PW_D3D9_HELLO) || (state==PW_D3D9_STARTING && record.opcode!=PW_D3D9_HELLO) || !opcode(record.opcode) || (!!record.object != !!record.generation) || record.sequence!=c->next_receive || record.sequence==UINT64_MAX || record.ticket!=record.sequence)return bad(c);
    if(record.opcode==PW_D3D9_STOP && state!=PW_D3D9_STOPPING &&
       !(c->role==PW_D3D9_CLIENT && state==PW_D3D9_STOPPED))return bad(c);
    if(c->role==PW_D3D9_SERVICE && c->next_receive>1 &&
       c->pending[(c->next_receive-2)%PW_D3D9_WIRE_PENDING].opcode==PW_D3D9_STOP)return bad(c);
    if(c->role==PW_D3D9_CLIENT) {
        if(!c->pending_count || !same(&record,&c->pending[(c->next_receive-1)%PW_D3D9_WIRE_PENDING]))return bad(c);
    } else {
        if(record.result)return bad(c);
        if(c->pending_count==PW_D3D9_WIRE_PENDING)return PW_D3D9_FULL;
    }
    if(bytes<total)return PW_D3D9_SMALL;
    copy(c,ring,read,scratch,total,0);
    if(memcmp(header,scratch,sizeof(header)))return bad(c);
    for(i=PW_D3D9_WIRE_HEADER+payload;i<total;i++)if(wire[i])return bad(c);
    if(c->role==PW_D3D9_SERVICE){c->pending[(c->next_receive-1)%PW_D3D9_WIRE_PENDING]=record;c->pending_count++;}else c->pending_count--;
    *m=record;c->next_receive++;atomic_store_explicit(&s->read[ring],read+total,memory_order_release);return PW_D3D9_OK;
}
int pw_d3d9_wire_slice(uint32_t bytes,uint32_t fixed,uint32_t offset,uint32_t length,uint32_t alignment)
{
    if(!alignment || (alignment&(alignment-1)) || fixed>bytes)return PW_D3D9_INVALID;
    if(!length)return offset ? PW_D3D9_INVALID : PW_D3D9_OK;
    return offset>=fixed && !(offset&(alignment-1)) && offset<=bytes && length<=bytes-offset ? PW_D3D9_OK : PW_D3D9_INVALID;
}
