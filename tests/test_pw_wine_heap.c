/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../wine/ps5/pw_wine_heap.h"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

enum { THREADS=8,ROUNDS=20000 };

/* A backing that hands out its own mappings: what the console's direct memory does. */
#include <sys/mman.h>
static unsigned backed_maps,backed_unmaps,refuse_next;
static void *backed[64];static size_t backed_bytes[64];
static void *backing_map(void *context,size_t bytes)
{
    assert(context==&backed_maps);
    if(refuse_next){refuse_next=0;return NULL;}
    void *p=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);assert(p!=MAP_FAILED);
    backed[backed_maps%64]=p;backed_bytes[backed_maps%64]=bytes;backed_maps++;return p;
}
static int backing_unmap(void *context,void *address,size_t bytes)
{
    assert(context==&backed_maps);
    for(unsigned i=0;i<64;i++)if(backed[i]==address){assert(backed_bytes[i]==bytes);backed[i]=NULL;munmap(address,bytes);backed_unmaps++;return 0;}
    return 1;
}


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
            size_t alignment=(size_t)32<<((seed>>4)%9);   /* 32 .. 8192 */
            uint8_t *bytes=(seed&2)?pw_wine_heap_memalign(alignment,size):pw_wine_heap_malloc(size);
            assert(bytes && (!(seed&2) || !((uintptr_t)bytes&(alignment-1))));
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
    /* Aligned blocks, small and large: aligned, writable, freed like others. */
    static const size_t alignments[]={32,64,256,4096,16384,65536};
    static const size_t sizes[]={1,48,3000,70000,300000};
    for(size_t i=0;i<sizeof(alignments)/sizeof(alignments[0]);i++)
        for(size_t j=0;j<sizeof(sizes)/sizeof(sizes[0]);j++) {
            uint8_t *aligned=pw_wine_heap_memalign(alignments[i],sizes[j]);
            assert(aligned && !((uintptr_t)aligned&(alignments[i]-1)));
            memset(aligned,0x5a,sizes[j]);
            assert(pw_wine_heap_usable_size(aligned)==sizes[j]);
            pw_wine_heap_free(aligned);
        }
    pw_wine_heap_stats(&s);assert(s.live_bytes==0 && s.foreign_frees==4 && !s.large_live);
    /* 16 bytes or less is an ordinary block; other alignments are refused. */
    uint8_t *plain=pw_wine_heap_memalign(16,40);assert(plain && pw_wine_heap_usable_size(plain)==48);
    pw_wine_heap_free(plain);
    assert(!pw_wine_heap_memalign(0,8) && !pw_wine_heap_memalign(24,8) && !pw_wine_heap_memalign(64,SIZE_MAX-8));
    /* realloc moves an aligned block to an ordinary one with its bytes. */
    uint8_t *page=pw_wine_heap_memalign(4096,100);assert(page);
    for(int i=0;i<100;i++)page[i]=(uint8_t)(i*3);
    uint8_t *grown=pw_wine_heap_realloc(page,9000);assert(grown);
    for(int i=0;i<100;i++)assert(grown[i]==(uint8_t)(i*3));
    pw_wine_heap_free(grown);
    /* A freed aligned block's pointer is foreign afterwards, like any double free. */
    uint8_t *twice=pw_wine_heap_memalign(128,32);assert(twice);
    pw_wine_heap_free(twice);pw_wine_heap_free(twice);
    pw_wine_heap_stats(&s);assert(s.foreign_frees==5 && s.live_bytes==0);
    /* Concurrent use keeps every block intact and returns to zero live bytes. */
    pthread_t threads[THREADS];
    for(uintptr_t t=0;t<THREADS;t++)assert(!pthread_create(&threads[t],NULL,worker,(void *)(t+1)));
    for(unsigned t=0;t<THREADS;t++)assert(!pthread_join(threads[t],NULL));
    pw_wine_heap_stats(&s);
    assert(s.live_bytes==0 && !s.large_live && s.allocations==s.frees && s.peak_live_bytes>1000000);
    /* A backing gets new large blocks and spans; free gives large ones back to it,
     * a refusal falls back to an anonymous mapping, and blocks mapped before the
     * backing was installed are still unmapped as before. */
    uint8_t *before=pw_wine_heap_malloc(100000);assert(before);
    const PwWineHeapBacking backing={&backed_maps,backing_map,backing_unmap};
    pw_wine_heap_set_backing(&backing);
    uint8_t *big=pw_wine_heap_malloc(150000);assert(big && backed_maps==1);memset(big,1,150000);
    refuse_next=1;uint8_t *fallback=pw_wine_heap_malloc(150000);assert(fallback && backed_maps==1);
    pw_wine_heap_free(big);assert(backed_unmaps==1);
    pw_wine_heap_free(fallback);pw_wine_heap_free(before);assert(backed_unmaps==1);
    void *small[40000];unsigned spans_before=backed_maps;
    for(unsigned i=0;i<40000;i++){small[i]=pw_wine_heap_malloc(48);assert(small[i]);}
    assert(backed_maps>spans_before);
    for(unsigned i=0;i<40000;i++)pw_wine_heap_free(small[i]);
    pw_wine_heap_set_backing(NULL);
    pw_wine_heap_stats(&s);assert(s.live_bytes==0 && !s.large_live);
    return 0;
}
