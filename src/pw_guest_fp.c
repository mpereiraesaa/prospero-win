/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_guest_fp.h"
#include "../include/prospero_win.h"
#include <string.h>
/* MSVCRT flag encodings, reviewed against pinned Wine msvcrt/math.c and
 * include/msvcrt/float.h. This is guest integer state, not host FP calls. */
enum { EM=0x8001f, DENORMAL=0x80000, RC=0x300, PC=0x30000,
       IC=0x40000, DN=0x3000000 };
static const uint32_t exception_bits[]={0x10,0x80000,8,4,2,1};
static uint32_t decode(uint32_t raw,unsigned sse)
{
    uint32_t flags=0;
    for(unsigned i=0;i<6;i++)if(raw&(1u<<(i+(sse?7:0))))flags|=exception_bits[i];
    flags|=((raw>>(sse?13:10))&3)<<8;
    if(sse) {
        unsigned mode=raw&0x8040;
        flags|=mode==0x8040?0x1000000:mode==0x40?0x2000000:mode==0x8000?0x3000000:0;
    } else {
        unsigned precision=raw&0x300;
        flags|=precision==0?0x20000:precision==0x200?0x10000:0;
        if(raw&0x1000)flags|=IC;
    }
    return flags;
}
static uint32_t encode(uint32_t raw,uint32_t flags,unsigned sse)
{
    uint32_t next=raw&~(sse?0xffc0u:0x1f3fu);
    for(unsigned i=0;i<6;i++)if(flags&exception_bits[i])next|=1u<<(i+(sse?7:0));
    next|=((flags>>8)&3)<<(sse?13:10);
    if(sse) {
        unsigned mode=flags&DN;
        next|=mode==0x1000000?0x8040:mode==0x2000000?0x40:mode==0x3000000?0x8000:0;
        if(next!=raw)next&=~0x3fu; /* Wine clears SSE status on control change. */
    } else {
        unsigned precision=flags&PC;
        next|=precision==0?0x300:precision==0x10000?0x200:0;
        if(flags&IC)next|=0x1000;
    }
    return next;
}
void pw_guest_fp_init(PwGuestFp *fp)
{
    if(fp)*fp=(PwGuestFp){.x87_control=0x027f,.x87_tag=0xffff,
                         .mxcsr=0x1f80,.initialized=1};
}
int pw_guest_fp_control(PwGuestFp *fp,uint32_t value,uint32_t mask,uint32_t *result)
{
    if(!fp || !result)return PW_ERR_PRECONDITION;
    if(!fp->initialized)return PW_ERR_STATE;
    mask&=~DENORMAL; /* _controlfp, unlike _control87, cannot change this mask. */
    uint32_t xmask=mask&(EM|RC|PC|IC),smask=mask&(EM|RC|DN);
    uint32_t x=decode(fp->x87_control,0),s=decode(fp->mxcsr,1);
    x=(x&~xmask)|(value&xmask);s=(s&~smask)|(value&smask);
    fp->x87_control=(uint16_t)encode(fp->x87_control,x,0);
    fp->mxcsr=encode(fp->mxcsr,s,1);
    *result=x|s|(((x^s)&(EM|RC))?0x80000000u:0);
    return PW_OK;
}
static unsigned top(const PwGuestFp *fp){return (fp->x87_status>>11)&7;}
static unsigned tag(const PwGuestFp *fp,unsigned slot){return (fp->x87_tag>>(slot*2))&3;}
static void set_tag(PwGuestFp *fp,unsigned slot,unsigned value)
{
    fp->x87_tag=(uint16_t)((fp->x87_tag&~(3u<<(slot*2)))|(value<<(slot*2)));
}
static unsigned classify80(const uint8_t value[10])
{
    uint64_t significand=0;
    for(unsigned i=0;i<8;i++)significand|=(uint64_t)value[i]<<(i*8);
    unsigned exponent=((unsigned)value[8]|(unsigned)value[9]<<8)&0x7fff;
    if(!exponent && !significand)return 1; /* zero */
    if(!exponent || exponent==0x7fff || !(significand>>63))return 2; /* special */
    return 0; /* finite normalized */
}
int pw_guest_x87_push(PwGuestFp *fp,const uint8_t value[10])
{
    if(!fp || !value)return PW_ERR_PRECONDITION;
    if(!fp->initialized)return PW_ERR_STATE;
    unsigned slot=(top(fp)-1)&7;
    if(tag(fp,slot)!=3)return PW_ERR_LIMIT;
    memcpy(fp->x87_st[slot],value,10);set_tag(fp,slot,classify80(value));
    fp->x87_status=(uint16_t)((fp->x87_status&~0x3800u)|(slot<<11));return PW_OK;
}
int pw_guest_x87_peek(const PwGuestFp *fp,unsigned logical_index,uint8_t value[10])
{
    if(!fp || !value || logical_index>=8)return PW_ERR_PRECONDITION;
    if(!fp->initialized)return PW_ERR_STATE;
    unsigned slot=(top(fp)+logical_index)&7;
    if(tag(fp,slot)==3)return PW_ERR_NOT_FOUND;
    memcpy(value,fp->x87_st[slot],10);return PW_OK;
}
int pw_guest_x87_pop(PwGuestFp *fp,uint8_t value[10])
{
    if(!fp || !value)return PW_ERR_PRECONDITION;
    if(!fp->initialized)return PW_ERR_STATE;
    unsigned slot=top(fp);
    if(tag(fp,slot)==3)return PW_ERR_NOT_FOUND;
    memcpy(value,fp->x87_st[slot],10);memset(fp->x87_st[slot],0,10);set_tag(fp,slot,3);
    fp->x87_status=(uint16_t)((fp->x87_status&~0x3800u)|(((slot+1)&7)<<11));return PW_OK;
}

static void put16(uint8_t *p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void put32(uint8_t *p,uint32_t v){put16(p,v);put16(p+2,v>>16);}
static uint32_t get16(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8;}
static uint32_t get32(const uint8_t *p){return get16(p)|get16(p+2)<<16;}
/* The full tag word, every register given a valid tag reclassified. */
static uint16_t full_tags(const PwGuestFp *fp,unsigned valid_mask)
{
    uint16_t tags=0;
    for(unsigned slot=0;slot<8;slot++)
        tags|=(uint16_t)((valid_mask>>slot&1?classify80(fp->x87_st[slot]):3u)<<(slot*2));
    return tags;
}
void pw_guest_fp_to_fxsave(const PwGuestFp *fp,uint8_t out[PW_GUEST_FXSAVE_BYTES])
{
    memset(out,0,PW_GUEST_FXSAVE_BYTES);
    put16(out,fp->x87_control);put16(out+2,fp->x87_status);
    for(unsigned slot=0;slot<8;slot++)if(tag(fp,slot)!=3)out[4]|=(uint8_t)(1u<<slot);
    put16(out+6,fp->x87_opcode&0x7ffu);put32(out+8,fp->x87_ip);put32(out+16,fp->x87_dp);
    put32(out+24,fp->mxcsr);put32(out+28,0xffff);
    for(unsigned i=0;i<8;i++)memcpy(out+32+i*16,fp->x87_st[(top(fp)+i)&7],10);
    memcpy(out+160,fp->xmm,sizeof(fp->xmm));
}
void pw_guest_fp_from_fxsave(PwGuestFp *fp,const uint8_t in[PW_GUEST_FXSAVE_BYTES])
{
    fp->x87_control=(uint16_t)get16(in);fp->x87_status=(uint16_t)get16(in+2);
    fp->x87_opcode=(uint16_t)(get16(in+6)&0x7ffu);fp->x87_ip=get32(in+8);fp->x87_dp=get32(in+16);
    fp->mxcsr=get32(in+24);
    for(unsigned i=0;i<8;i++)memcpy(fp->x87_st[(top(fp)+i)&7],in+32+i*16,10);
    memcpy(fp->xmm,in+160,sizeof(fp->xmm));
    fp->x87_tag=full_tags(fp,in[4]);fp->initialized=1;
}
