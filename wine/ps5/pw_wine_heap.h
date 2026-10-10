/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_WINE_HEAP_H
#define PW_WINE_HEAP_H
#include <stddef.h>
#include <stdint.h>

/* Heap for Wine's Unix side on the PS5, where the title's libc malloc stops
 * near 13 MiB. Small blocks (up to 64 KiB) come from power-of-two size
 * classes carved out of 1 MiB anonymous mappings; larger blocks get their
 * own mapping and are unmapped on free. One mutex serialises every call.
 * A pointer this heap did not return is never touched: free counts it as
 * foreign and returns, so memory allocated inside libc cannot corrupt it. */
typedef struct PwWineHeapStats {
    uint64_t live_bytes,peak_live_bytes,mapped_bytes,peak_mapped_bytes;
    uint64_t allocations,frees,failures,foreign_frees,large_live;
} PwWineHeapStats;

void *pw_wine_heap_malloc(size_t bytes);
void *pw_wine_heap_calloc(size_t count,size_t bytes);
void *pw_wine_heap_realloc(void *pointer,size_t bytes);
void pw_wine_heap_free(void *pointer);
/* A block aligned to alignment, a power of two; free and realloc take it
 * like any other. NULL for a bad alignment or no memory. */
void *pw_wine_heap_memalign(size_t alignment,size_t bytes);
/* Usable bytes of a block this heap returned, 0 for anything else. */
size_t pw_wine_heap_usable_size(const void *pointer);
void pw_wine_heap_stats(PwWineHeapStats *stats);

/* Where new spans and large blocks come from. By default anonymous mappings;
 * on the console those are flexible memory, of which a title has about
 * 440 MiB, so pw_wine_dmem_ps5.c installs direct memory instead. map returns
 * NULL to fall back to an anonymous mapping; unmap returns 0 when it owned
 * the range and nonzero to have it unmapped as an anonymous mapping. Both
 * are called with the heap's lock held and must not allocate from it. */
typedef struct PwWineHeapBacking {
    void *context;
    void *(*map)(void *context,size_t bytes);
    int (*unmap)(void *context,void *address,size_t bytes);
} PwWineHeapBacking;
void pw_wine_heap_set_backing(const PwWineHeapBacking *backing);

#endif
