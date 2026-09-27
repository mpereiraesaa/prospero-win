/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_pad.h"
#include <limits.h>
#include <string.h>

static void advance(PwPad *pad,uint32_t next)
{
    uint32_t changed=pad->previous_buttons^next;
    pad->pressed_edges|=changed&next;
    pad->released_edges|=changed&~next;
    pad->previous_buttons=next;
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
