/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The C allocation interface of Wine's Unix side on PS5, served by
 * pw_wine_heap. ntdll.so links this file and exports it, so win32u.so binds
 * its malloc family to the same heap; the in-process wineserver links its
 * own copy. strdup and asprintf are defined here too, because the libc
 * versions would return memory from the title's small libc heap. */
#include "pw_wine_heap.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void *malloc(size_t bytes){return pw_wine_heap_malloc(bytes);}
void *calloc(size_t count,size_t bytes){return pw_wine_heap_calloc(count,bytes);}
void *realloc(void *pointer,size_t bytes){return pw_wine_heap_realloc(pointer,bytes);}
void free(void *pointer){pw_wine_heap_free(pointer);}
/* Mesa, linked into win32u with the PS5 OpenGL SDK, allocates its contexts
 * aligned; libc's versions would take them from the title's small heap. */
void *memalign(size_t alignment,size_t bytes){return pw_wine_heap_memalign(alignment,bytes);}
void *aligned_alloc(size_t alignment,size_t bytes){return pw_wine_heap_memalign(alignment,bytes);}
int posix_memalign(void **out,size_t alignment,size_t bytes)
{
    if(!alignment || (alignment&(alignment-1)) || alignment%sizeof(void *))return EINVAL;
    void *pointer=pw_wine_heap_memalign(alignment,bytes);
    if(!pointer)return ENOMEM;
    *out=pointer;return 0;
}

char *strdup(const char *text)
{
    size_t bytes=strlen(text)+1;char *copy=pw_wine_heap_malloc(bytes);
    if(copy)memcpy(copy,text,bytes);
    return copy;
}
char *strndup(const char *text,size_t limit)
{
    size_t bytes=strnlen(text,limit);char *copy=pw_wine_heap_malloc(bytes+1);
    if(copy){memcpy(copy,text,bytes);copy[bytes]=0;}
    return copy;
}
int vasprintf(char **out,const char *format,va_list arguments)
{
    va_list measure;va_copy(measure,arguments);
    int length=vsnprintf(NULL,0,format,measure);va_end(measure);
    if(length<0)return -1;
    char *text=pw_wine_heap_malloc((size_t)length+1);
    if(!text)return -1;
    (void)vsnprintf(text,(size_t)length+1,format,arguments);
    *out=text;return length;
}
int asprintf(char **out,const char *format,...)
{
    va_list arguments;va_start(arguments,format);
    int length=vasprintf(out,format,arguments);va_end(arguments);
    return length;
}
