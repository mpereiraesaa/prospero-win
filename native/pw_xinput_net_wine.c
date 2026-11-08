/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_xinput_net_wine.h"
#include <stdio.h>
#include <string.h>

enum { PW_XINPUT_NET_BATCH=64, PW_XINPUT_NET_REJECT_LOGS=3 };

static void say(PwXinputNetWine *bridge,const char *line) { if(bridge->log)bridge->log(line); }

int pw_xinput_net_wine_init(PwXinputNetWine *bridge,const PwXinputNetOps *ops,
                            PwXinputNetSetSlot set_slot,void (*log)(const char *))
{
    char line[96];
    if(!bridge)return -1;
    memset(bridge,0,sizeof(*bridge));bridge->fd=-1;bridge->reserved=1u;
    bridge->set_slot=set_slot;bridge->log=log;
    pw_xinput_net_init(&bridge->net);
    if(!set_slot || !ops || !ops->open || !ops->receive)return 0;
    bridge->ops=*ops;
    int fd=ops->open(ops->ctx,PW_XINPUT_NET_PORT);
    if(fd<0) {
        snprintf(line,sizeof(line),"netxinput unavailable port=%u errno=%d",PW_XINPUT_NET_PORT,-fd);
        say(bridge,line);
        return fd;
    }
    bridge->fd=fd;
    snprintf(line,sizeof(line),"netxinput listening 127.0.0.1:%u",PW_XINPUT_NET_PORT);
    say(bridge,line);
    return 0;
}

static void publish(PwXinputNetWine *bridge,unsigned source)
{
    PwWinePad neutral={0};
    (void)bridge->set_slot(bridge->net.source[source].slot,
                           bridge->blocked?&neutral:&bridge->pad[source],&bridge->caps[source]);
}

static void clear(PwXinputNetWine *bridge,unsigned source,uint32_t slot,const char *why)
{
    char line[96];
    (void)bridge->set_slot(slot,NULL,NULL);
    snprintf(line,sizeof(line),"netxinput source=%u gone xinput=%u (%s)",source,slot,why);
    say(bridge,line);
}

void pw_xinput_net_wine_reserve(PwXinputNetWine *bridge,uint32_t users)
{
    if(!bridge)return;
    bridge->reserved=users|1u;
    for(unsigned i=0;i<PW_XINPUT_NET_SOURCES;i++) {
        PwXinputNetSource *source=&bridge->net.source[i];
        if(!source->active || !(bridge->reserved&(1u<<source->slot)))continue;
        source->active=0;
        clear(bridge,i,source->slot,"reserved");
    }
}

void pw_xinput_net_wine_block(PwXinputNetWine *bridge,int blocked)
{
    if(!bridge || bridge->blocked==(unsigned)(blocked!=0))return;
    bridge->blocked=blocked!=0;
    for(unsigned i=0;i<PW_XINPUT_NET_SOURCES;i++)
        if(bridge->net.source[i].active)publish(bridge,i);
}

uint32_t pw_xinput_net_wine_users(const PwXinputNetWine *bridge)
{
    uint32_t users=0;
    if(!bridge)return 0;
    for(unsigned i=0;i<PW_XINPUT_NET_SOURCES;i++)
        if(bridge->net.source[i].active)users|=1u<<bridge->net.source[i].slot;
    return users;
}

void pw_xinput_net_wine_poll(PwXinputNetWine *bridge,uint64_t now_ms)
{
    uint8_t data[PW_XINPUT_NET_SIZE+1];
    char line[128];
    uint32_t slot;
    if(!bridge || bridge->fd<0)return;
    for(int i=0;i<PW_XINPUT_NET_BATCH;i++) {
        long n=bridge->ops.receive(bridge->ops.ctx,bridge->fd,data,sizeof(data));
        if(n==0)break;
        if(n<0) {
            snprintf(line,sizeof(line),"netxinput receive errno=%ld, stopped",-n);say(bridge,line);
            pw_xinput_net_wine_close(bridge);
            return;
        }
        PwXinputNetMessage message;
        if(pw_xinput_net_parse(data,(size_t)n,&message)) {
            if(bridge->rejected++<PW_XINPUT_NET_REJECT_LOGS) {
                snprintf(line,sizeof(line),"netxinput rejected bytes=%ld",n);say(bridge,line);
            }
            continue;
        }
        bridge->received++;
        uint8_t busy[PW_XINPUT_NET_SLOTS];
        for(uint32_t s=0;s<PW_XINPUT_NET_SLOTS;s++)busy[s]=(bridge->reserved>>s)&1u;
        int held=bridge->net.source[message.index].active;
        int action=pw_xinput_net_accept(&bridge->net,&message,now_ms,busy,&slot);
        if(action==PW_XINPUT_NET_CLEAR) {
            clear(bridge,message.index,slot,"message");
        } else if(action==PW_XINPUT_NET_SET) {
            PwWinePad *pad=&bridge->pad[message.index];
            memset(pad,0,sizeof(*pad));
            pad->buttons=message.buttons;pad->left_trigger=message.left_trigger;pad->right_trigger=message.right_trigger;
            pad->thumb_lx=message.thumb_lx;pad->thumb_ly=message.thumb_ly;
            pad->thumb_rx=message.thumb_rx;pad->thumb_ry=message.thumb_ry;
            bridge->caps[message.index]=(PwWinePadCaps){message.subtype,message.vendor_id,message.product_id,message.revision,0};
            publish(bridge,message.index);
            if(!held) {
                snprintf(line,sizeof(line),"netxinput source=%u xinput=%u subtype=%u vid=%04x pid=%04x",
                         message.index,slot,message.subtype,message.vendor_id,message.product_id);
                say(bridge,line);
            }
        } else if(message.connected && !held) {
            snprintf(line,sizeof(line),"netxinput source=%u no free xinput slot",message.index);
            say(bridge,line);
        }
    }
    unsigned source;
    while(pw_xinput_net_expire(&bridge->net,now_ms,&slot,&source))clear(bridge,source,slot,"timeout");
}

void pw_xinput_net_wine_close(PwXinputNetWine *bridge)
{
    if(!bridge)return;
    for(unsigned i=0;i<PW_XINPUT_NET_SOURCES;i++)
        if(bridge->net.source[i].active) {
            bridge->net.source[i].active=0;
            if(bridge->set_slot)(void)bridge->set_slot(bridge->net.source[i].slot,NULL,NULL);
        }
    if(bridge->fd>=0 && bridge->ops.close)bridge->ops.close(bridge->ops.ctx,bridge->fd);
    bridge->fd=-1;
}
