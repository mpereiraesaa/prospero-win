/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_pad.h"
#include <limits.h>
#include <string.h>

static unsigned transitions(const PwPad *pad,uint32_t next)
{
    uint32_t changed=pad->previous_buttons^next;unsigned count=0;
    for(size_t i=0;i<pad->map_count;i++)if(changed&pad->map[i].mask)count++;
    return count;
}

static void advance(PwPad *pad,uint32_t next)
{
    uint32_t changed=pad->previous_buttons^next;
    pad->pressed_edges|=changed&next;
    pad->released_edges|=changed&~next;
    pad->previous_buttons=next;
}

static int emit(PwPad *pad,PwUser32 *user,uint32_t window,uint32_t next,
                uint64_t timestamp_us)
{
    unsigned needed=transitions(pad,next);
    if(needed>PW_USER32_QUEUE_CAPACITY-user->queue_count)return PW_ERR_LIMIT;
    uint32_t changed=pad->previous_buttons^next;
    for(size_t i=0;i<pad->map_count;i++) {
        const PwPadKeyMap *binding=&pad->map[i];
        if(!(changed&binding->mask))continue;
        unsigned down=(next&binding->mask)!=0;
        int status=pw_user32_post_key(user,window,binding->virtual_key,
            binding->scan_code,binding->extended,down,(uint32_t)(timestamp_us/1000u));
        if(status!=PW_OK)return status;
        pad->stats.events++;if(down)pad->stats.presses++;else pad->stats.releases++;
    }
    advance(pad,next);return PW_OK;
}

int pw_pad_init(PwPad *pad,const PwPadKeyMap *map,size_t count)
{
    if(!pad || !map || !count || count>32)return PW_ERR_PRECONDITION;
    uint32_t masks=0;
    for(size_t i=0;i<count;i++) {
        if(!map[i].mask || (masks&map[i].mask) || !map[i].virtual_key ||
           !map[i].action || !*map[i].action || map[i].extended>1)
            return PW_ERR_PRECONDITION;
        masks|=map[i].mask;
    }
    memset(pad,0,sizeof(*pad));pad->map=map;pad->map_count=count;return PW_OK;
}

int pw_pad_neutralize(PwPad *pad,PwUser32 *user,uint32_t window,uint64_t timestamp_us)
{
    if(!pad || !user || !window)return PW_ERR_PRECONDITION;
    if(!pad->previous_buttons){pad->connected=0;return PW_OK;}
    int status=emit(pad,user,window,0,timestamp_us);
    if(status==PW_OK){pad->connected=0;pad->stats.neutralizations++;}
    return status;
}

int pw_pad_process(PwPad *pad,PwUser32 *user,uint32_t window,
                   const PwPadSample *samples,size_t count)
{
    if(!pad || !pad->map || !user || !window || (!samples && count) || count>INT_MAX)
        return PW_ERR_PRECONDITION;
    pad->pressed_edges=0;pad->released_edges=0;
    pad->stats.batches++;if(count>pad->stats.max_batch)pad->stats.max_batch=(uint32_t)count;
    for(size_t i=0;i<count;i++) {
        const PwPadSample *sample=&samples[i];pad->stats.samples++;
        if(!sample->connected || sample->intercepted) {
            int status=pw_pad_neutralize(pad,user,window,sample->timestamp_us);
            if(status!=PW_OK)return status;
            continue;
        }
        if(pad->generation_valid && sample->generation!=pad->generation) {
            int status=pw_pad_neutralize(pad,user,window,sample->timestamp_us);
            if(status!=PW_OK)return status;
        }
        pad->generation=sample->generation;pad->generation_valid=1;pad->connected=1;
        int status=emit(pad,user,window,sample->buttons,sample->timestamp_us);
        if(status!=PW_OK)return status;
    }
    return PW_OK;
}

int pw_pad_track(PwPad *pad,const PwPadSample *samples,size_t count)
{
    if(!pad || (!samples && count) || count>INT_MAX)return PW_ERR_PRECONDITION;
    pad->pressed_edges=0;pad->released_edges=0;
    pad->stats.batches++;if(count>pad->stats.max_batch)pad->stats.max_batch=(uint32_t)count;
    for(size_t i=0;i<count;i++) {
        const PwPadSample *sample=&samples[i];pad->stats.samples++;
        if(!sample->connected || sample->intercepted){advance(pad,0);pad->connected=0;continue;}
        if(pad->generation_valid && sample->generation!=pad->generation)advance(pad,0);
        pad->generation=sample->generation;pad->generation_valid=1;pad->connected=1;
        advance(pad,sample->buttons);
    }
    return PW_OK;
}
