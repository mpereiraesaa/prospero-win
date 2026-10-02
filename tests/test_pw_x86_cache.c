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
    assert(cache.occupied==1);

    /* Three quarters of the entries, then refused until a reset. */
    PwX86CacheEntry many[8];
    PwX86Block small={.source_bytes=1,.code_bytes=16,.instructions=1};
    assert(pw_x86_cache_init(&cache,many,8,1024,1)==PW_OK && cache.occupied==0);
    for(uint32_t i=0;i<6;i++)
        assert(pw_x86_cache_publish(&cache,0x1000+i*0x10,&small,cache.cursor,&entry)==PW_OK);
    assert(cache.occupied==6);
    assert(pw_x86_cache_publish(&cache,0x2000,&small,cache.cursor,&entry)==PW_ERR_LIMIT);
    assert(cache.occupied==6 && cache.publishes==6);
    assert(pw_x86_cache_lookup(&cache,0x1050,&entry)==PW_OK);
    assert(pw_x86_cache_lookup(&cache,0x2000,&entry)==PW_ERR_NOT_FOUND);
    assert(pw_x86_cache_reset(&cache,2)==PW_OK && cache.occupied==0);
    assert(pw_x86_cache_publish(&cache,0x2000,&small,0,&entry)==PW_OK && cache.occupied==1);
    return 0;
}
