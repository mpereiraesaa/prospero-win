/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_x86_cache.h"
#include <assert.h>

int main(void)
{
    PwX86Cache cache;PwX86CacheEntry entries[2];const PwX86CacheEntry *entry=(void *)1;
    assert(pw_x86_cache_init(&cache,entries,2,128,7)==PW_OK);
    assert(pw_x86_cache_lookup(&cache,0x1000,&entry)==PW_ERR_NOT_FOUND && !entry);
    PwX86Block first={.source_bytes=7,.code_bytes=17,.instructions=3};
    assert(pw_x86_cache_publish(&cache,0x1000,&first,0,&entry)==PW_OK);
    assert(entry->generation==7 && entry->code_offset==0 && entry->instructions==3);
    assert(cache.cursor==32 && cache.publishes==1);
    assert(pw_x86_cache_lookup(&cache,0x1000,&entry)==PW_OK && entry->source_bytes==7);
    assert(cache.hits==1 && cache.misses==1);
    assert(pw_x86_cache_publish(&cache,0x1000,&first,32,&entry)==PW_ERR_STATE);
    PwX86Block second={.source_bytes=2,.code_bytes=80,.instructions=1};
    assert(pw_x86_cache_publish(&cache,0x3000,&second,31,&entry)==PW_ERR_LIMIT);
    assert(pw_x86_cache_publish(&cache,0x3000,&second,32,&entry)==PW_OK && cache.cursor==112);
    assert(pw_x86_cache_lookup(&cache,0x3000,&entry)==PW_OK && cache.max_probe==2);
    assert(pw_x86_cache_publish(&cache,0x2000,&first,112,&entry)==PW_ERR_LIMIT);
    assert(pw_x86_cache_reset(&cache,8)==PW_OK && cache.cursor==0 && cache.resets==1);
    assert(pw_x86_cache_lookup(&cache,0x1000,&entry)==PW_ERR_NOT_FOUND);
    assert(pw_x86_cache_reset(&cache,8)==PW_ERR_PRECONDITION);
    assert(pw_x86_cache_publish(&cache,0x1000,&first,0,&entry)==PW_OK && entry->generation==8);
    {
        PwX86CacheEntry sparse[8];
        assert(pw_x86_cache_init(&cache,sparse,8,128,1)==PW_OK);
        assert(pw_x86_cache_publish(&cache,0x1000,&first,0,&entry)==PW_OK);
        uint32_t bucket=((uint32_t)(entry-sparse)+1)%8;
        pw_x86_cache_touch(&cache,bucket);
        uint32_t head=cache.reset_head;
        pw_x86_cache_touch(&cache,bucket);
        assert(cache.reset_head==head); /* Duplicate registration cannot cycle. */
        sparse[bucket].pending_head=123;
        assert(!sparse[bucket].used);
        uint32_t pc=1;
        for(;;pc++) {
            uint32_t hash=pc*2654435761u;hash^=hash>>16;
            if(hash%8==bucket)break;
        }
        uint32_t next=sparse[bucket].reset_next;
        assert(pw_x86_cache_publish(&cache,pc,&first,32,&entry)==PW_OK);
        assert(entry==&sparse[bucket] && entry->pending_head==123);
        assert(cache.reset_head==head && entry->reset_next==next);
        uint32_t unpublished=(bucket+1)%8;
        pw_x86_cache_touch(&cache,unpublished);
        sparse[unpublished].pending_head=456;
        assert(!sparse[unpublished].used);
        assert(pw_x86_cache_reset(&cache,2)==PW_OK && !cache.reset_head);
        for(unsigned i=0;i<8;i++)
            assert(!sparse[i].used && !sparse[i].pending_head && !sparse[i].reset_tracked && !sparse[i].reset_next);
        assert(pw_x86_cache_publish(&cache,0x1000,&first,0,&entry)==PW_OK);
        assert(entry->generation==2);
    }
    return 0;
}
