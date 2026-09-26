/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../wine/ps5/pw_wine_heap.h"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

enum { THREADS=8,ROUNDS=20000 };

static void *worker(void *seed_pointer)
{
    unsigned seed=(unsigned)(uintptr_t)seed_pointer;void *live[64]={0};size_t sizes[64]={0};
    for(unsigned i=0;i<ROUNDS;i++) {
        seed=seed*1103515245u+12345u;unsigned slot=(seed>>8)%64;
        if(live[slot]) {
            const uint8_t *bytes=live[slot];
            for(size_t k=0;k<sizes[slot];k++)assert(bytes[k]==(uint8_t)(slot^k));
            pw_wine_heap_free(live[slot]);live[slot]=NULL;
        } else {
            size_t size=(seed>>16)%((seed&1)?200000u:3000u)+1;
            uint8_t *bytes=pw_wine_heap_malloc(size);assert(bytes);
            for(size_t k=0;k<size;k++)bytes[k]=(uint8_t)(slot^k);
            live[slot]=bytes;sizes[slot]=size;
        }
    }
    for(unsigned slot=0;slot<64;slot++)pw_wine_heap_free(live[slot]);
    return NULL;
}

int main(void)
{
    PwWineHeapStats s;pw_wine_heap_stats(&s);assert(!s.live_bytes && !s.allocations);
    /* Size classes, alignment and usable size. */
    uint8_t *a=pw_wine_heap_malloc(1),*b=pw_wine_heap_malloc(100),*c=pw_wine_heap_malloc(0);
    assert(a && b && c && !((uintptr_t)a&15) && !((uintptr_t)b&15));
    assert(pw_wine_heap_usable_size(a)==16 && pw_wine_heap_usable_size(b)==112);
    pw_wine_heap_stats(&s);assert(s.live_bytes==101 && s.allocations==3 && s.mapped_bytes==1u<<20);
    /* Freed blocks are reused from their class. */
    pw_wine_heap_free(b);uint8_t *b2=pw_wine_heap_malloc(90);assert(b2==b);
    /* Large blocks have their own mapping and are unmapped on free. */
    uint8_t *large=pw_wine_heap_malloc(200000);assert(large && !((uintptr_t)large&15));
    memset(large,0xab,200000);
    pw_wine_heap_stats(&s);assert(s.large_live==1 && s.mapped_bytes==(1u<<20)+212992);
    assert(pw_wine_heap_usable_size(large)>=200000);
    pw_wine_heap_free(large);pw_wine_heap_stats(&s);
    assert(!s.large_live && s.mapped_bytes==1u<<20 && s.peak_mapped_bytes==(1u<<20)+212992);
    /* realloc keeps small growth in place, moves across classes, keeps bytes. */
    for(int i=0;i<16;i++)a[i]=(uint8_t)i;
    assert(pw_wine_heap_realloc(a,12)==a);
    uint8_t *moved=pw_wine_heap_realloc(a,5000);assert(moved && moved!=a);
    for(int i=0;i<12;i++)assert(moved[i]==(uint8_t)i);
    uint8_t *huge=pw_wine_heap_realloc(moved,300000);assert(huge);
    for(int i=0;i<12;i++)assert(huge[i]==(uint8_t)i);
    uint8_t *shrunk=pw_wine_heap_realloc(huge,40);assert(shrunk);
    for(int i=0;i<12;i++)assert(shrunk[i]==(uint8_t)i);
    assert(!pw_wine_heap_realloc(shrunk,0));
    uint8_t *fresh=pw_wine_heap_realloc(NULL,24);assert(fresh);pw_wine_heap_free(fresh);
    /* calloc zeroes recycled memory and refuses overflow. */
    memset(b2,0xff,90);pw_wine_heap_free(b2);
    uint8_t *zero=pw_wine_heap_calloc(9,10);assert(zero==b2);
    for(int i=0;i<90;i++)assert(!zero[i]);
    pw_wine_heap_stats(&s);uint64_t failures=s.failures;
    assert(!pw_wine_heap_calloc(SIZE_MAX/2,4));
    assert(!pw_wine_heap_malloc(SIZE_MAX-8));
    pw_wine_heap_stats(&s);assert(s.failures==failures+2);
    /* Pointers the heap never returned are left alone and counted. */
    void *libc=malloc(64);uint8_t local[32];
    pw_wine_heap_free(libc);pw_wine_heap_free(local+16);pw_wine_heap_free(zero+16);
    assert(pw_wine_heap_usable_size(libc)==0 && !pw_wine_heap_realloc(libc,8));
    pw_wine_heap_stats(&s);assert(s.foreign_frees==3);free(libc);
    pw_wine_heap_free(zero);pw_wine_heap_free(zero);   /* double free is refused */
    pw_wine_heap_free(c);pw_wine_heap_free(NULL);
    pw_wine_heap_stats(&s);assert(s.foreign_frees==4 && s.live_bytes==0);
    /* Concurrent use keeps every block intact and returns to zero live bytes. */
    pthread_t threads[THREADS];
    for(uintptr_t t=0;t<THREADS;t++)assert(!pthread_create(&threads[t],NULL,worker,(void *)(t+1)));
    for(unsigned t=0;t<THREADS;t++)assert(!pthread_join(threads[t],NULL));
    pw_wine_heap_stats(&s);
    assert(s.live_bytes==0 && !s.large_live && s.allocations==s.frees && s.peak_live_bytes>1000000);
    return 0;
}
