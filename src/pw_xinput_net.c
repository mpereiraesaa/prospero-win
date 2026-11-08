/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_xinput_net.h"
#include <string.h>

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0]|p[1]<<8); }

int pw_xinput_net_parse(const uint8_t *data,size_t length,PwXinputNetMessage *out)
{
    if(!data || !out || length!=PW_XINPUT_NET_SIZE || memcmp(data,"PBXI",4))return -1;
    if(data[4]!=1 || data[5]>=PW_XINPUT_NET_SOURCES || (data[6]&~1u) || data[30] || data[31])return -1;
    /* XINPUT_DEVSUBTYPE: gamepad, guitar, guitar alternate, drum kit, guitar bass. */
    if(data[7]!=1 && data[7]!=6 && data[7]!=7 && data[7]!=8 && data[7]!=11)return -1;
    memset(out,0,sizeof(*out));
    out->index=data[5];out->connected=data[6]&1u;out->subtype=data[7];
    out->vendor_id=u16(data+8);out->product_id=u16(data+10);out->revision=u16(data+12);
    out->buttons=(uint16_t)(u16(data+14)&0xF3FFu);
    out->left_trigger=data[16];out->right_trigger=data[17];
    out->thumb_lx=(int16_t)u16(data+18);out->thumb_ly=(int16_t)u16(data+20);
    out->thumb_rx=(int16_t)u16(data+22);out->thumb_ry=(int16_t)u16(data+24);
    out->sequence=(uint32_t)u16(data+26)|(uint32_t)u16(data+28)<<16;
    return 0;
}

void pw_xinput_net_init(PwXinputNet *net)
{
    if(net)memset(net,0,sizeof(*net));
}

int pw_xinput_net_holds(const PwXinputNet *net,uint32_t slot)
{
    if(!net)return 0;
    for(unsigned i=0;i<PW_XINPUT_NET_SOURCES;i++)
        if(net->source[i].active && net->source[i].slot==slot)return 1;
    return 0;
}

int pw_xinput_net_accept(PwXinputNet *net,const PwXinputNetMessage *message,uint64_t now_ms,
                         const uint8_t busy[PW_XINPUT_NET_SLOTS],uint32_t *slot)
{
    if(!net || !message || !slot || message->index>=PW_XINPUT_NET_SOURCES)return PW_XINPUT_NET_IGNORE;
    PwXinputNetSource *source=&net->source[message->index];
    if(!message->connected) {
        if(!source->active)return PW_XINPUT_NET_IGNORE;
        source->active=0;*slot=source->slot;
        return PW_XINPUT_NET_CLEAR;
    }
    if(!source->active) {
        uint32_t chosen=0;
        for(uint32_t s=PW_XINPUT_NET_SLOTS-1;s>=1 && !chosen;s--)
            if(!(busy && busy[s]) && !pw_xinput_net_holds(net,s))chosen=s;
        if(!chosen)return PW_XINPUT_NET_IGNORE;
        source->active=1;source->slot=chosen;
    }
    source->last_ms=now_ms;*slot=source->slot;
    return PW_XINPUT_NET_SET;
}

int pw_xinput_net_expire(PwXinputNet *net,uint64_t now_ms,uint32_t *slot,unsigned *index)
{
    if(!net || !slot)return 0;
    for(unsigned i=0;i<PW_XINPUT_NET_SOURCES;i++) {
        if(!net->source[i].active || now_ms-net->source[i].last_ms<PW_XINPUT_NET_TIMEOUT_MS)continue;
        net->source[i].active=0;*slot=net->source[i].slot;
        if(index)*index=i;
        return 1;
    }
    return 0;
}
