/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * The pixel-address permutation is adapted from SDL's PS5 tilemap backend:
 * Copyright (C) 2026 John Törnblom <john.tornblom@gmail.com>.
 * SDL's zlib license permits use and modification; this is a small standalone
 * scalar presenter, not a copy of the SDL video driver.
 */
#include "pw_videoout_ps5.h"
#include "pw_videoout_tile.h"
#include "pw_videoout_layout.h"
#include <string.h>

enum { WIDTH=1920,HEIGHT=1080,FRAME_BYTES=0x1000000,MEMORY_BYTES=0x3000000 };
typedef struct VideoBuffer {void *data,*metadata,*reserved0,*reserved1;} VideoBuffer;
typedef struct VideoAttribute {uint8_t bytes[80];} VideoAttribute;
extern size_t sceKernelGetDirectMemorySize(void);
extern int sceKernelAllocateDirectMemory(int64_t,int64_t,size_t,size_t,int,int64_t *);
extern int sceKernelMapDirectMemory(void **,size_t,int,int,int64_t,size_t);
extern int sceKernelMunmap(void *,size_t);
extern int sceKernelReleaseDirectMemory(int64_t,size_t);
extern int sceVideoOutOpen(int32_t,int32_t,int32_t,const void *);
extern int sceVideoOutClose(int32_t);
extern int sceVideoOutUnregisterBuffers(int32_t,int32_t);
extern int sceVideoOutSetFlipRate(int32_t,int32_t);
extern void sceVideoOutSetBufferAttribute2(void *,uint64_t,uint32_t,uint32_t,uint32_t,
                                            uint64_t,uint32_t,uint64_t);
extern int sceVideoOutRegisterBuffers2(int32_t,int32_t,int32_t,void *,int32_t,void *,int32_t,void *);
extern int sceVideoOutWaitVblank(int32_t);
extern int sceSystemServiceHideSplashScreen(void);

/* Column and row parts of the tiled index, filled once at open. */
static PwVideoOutTiles tiles;

int pw_videoout_ps5_open(PwVideoOutPs5 *video)
{
    if(!video)return PW_ERR_PRECONDITION;memset(video,0,sizeof(*video));
    video->handle=-1;video->physical=-1;
    pw_videoout_tiles_init(&tiles);
    int status=pw_agc_ps5_open(&video->agc);if(status!=PW_OK)return status;
    video->handle=sceVideoOutOpen(0xff,0,0,NULL);if(video->handle<0)goto state_failed;
    size_t pool=sceKernelGetDirectMemorySize();if(pool<MEMORY_BYTES)goto limit_failed;
    if(sceKernelAllocateDirectMemory(0,(int64_t)pool,MEMORY_BYTES,0x200000,3,
                                     &video->physical)<0)goto vm_failed;
    video->allocated=1;
    if(sceKernelMapDirectMemory(&video->memory,MEMORY_BYTES,0x33,0,video->physical,0x200000)<0)
        goto vm_failed;
    video->mapped=1;
    video->bytes=MEMORY_BYTES;video->frame_bytes=FRAME_BYTES;
    VideoBuffer buffers[2]={{video->memory,0,0,0},
        {(uint8_t *)video->memory+FRAME_BYTES,0,0,0}};VideoAttribute attribute;
    memset(&attribute,0,sizeof(attribute));
    (void)sceVideoOutSetFlipRate(video->handle,0);
    sceVideoOutSetBufferAttribute2(&attribute,0x8000000022000000ull,0,WIDTH,HEIGHT,0,0,0);
    if(sceVideoOutRegisterBuffers2(video->handle,0,0,buffers,2,&attribute,0,NULL)<0)
        goto state_failed;
    video->buffers_registered=1;video->opened=1;
    (void)sceSystemServiceHideSplashScreen();return PW_OK;
limit_failed:
    (void)pw_videoout_ps5_close(video);return PW_ERR_LIMIT;
vm_failed:
    (void)pw_videoout_ps5_close(video);return PW_ERR_VM;
state_failed:
    (void)pw_videoout_ps5_close(video);return PW_ERR_STATE;
}
int pw_videoout_ps5_close(PwVideoOutPs5 *video)
{
    if(!video)return PW_ERR_PRECONDITION;
    int status=PW_OK;
    video->unregister_rc=video->close_rc=video->munmap_rc=video->release_rc=0;
    if(video->buffers_registered) {
        video->unregister_rc=sceVideoOutUnregisterBuffers(video->handle,0);
        /* On FW 12.02, 0x80290009 is followed by a successful handle close,
         * unmap and direct-memory release. Preserve it as the measured
         * deferred-close case; other unregister errors still fail closed. */
        if(video->unregister_rc && (uint32_t)video->unregister_rc!=0x80290009u)
            status=PW_ERR_STATE;
        else video->buffers_registered=0;
    }
    if(video->handle>=0) {
        video->close_rc=sceVideoOutClose(video->handle);
        if(video->close_rc)status=PW_ERR_STATE;else video->handle=-1;
    }
    video->opened=0;
    if(video->mapped) {
        video->munmap_rc=sceKernelMunmap(video->memory,MEMORY_BYTES);
        if(video->munmap_rc)status=PW_ERR_STATE;else video->mapped=0;
    }
    if(video->allocated) {
        video->release_rc=sceKernelReleaseDirectMemory(video->physical,MEMORY_BYTES);
        if(video->release_rc)status=PW_ERR_STATE;else video->allocated=0;
    }
    video->agc_close_rc=pw_agc_ps5_close(&video->agc);
    if(video->agc_close_rc!=PW_OK)status=PW_ERR_STATE;
    video->memory=NULL;video->physical=-1;video->bytes=video->frame_bytes=0;
    return status;
}
int pw_videoout_ps5_present(PwVideoOutPs5 *video,const PwGdiTargetView *view)
{
    if(!video || !video->opened || !view || !view->pixels || !view->width || !view->height)
        return PW_ERR_PRECONDITION;
    unsigned index=video->index^1u;
    uint32_t *output=(uint32_t *)((uint8_t *)video->memory+2u*video->frame_bytes);
    void *scanout=(uint8_t *)video->memory+(size_t)index*video->frame_bytes;
    uint32_t background=0xff101018u;
    /* A target larger than the scanout is cropped, never placed at an
     * underflowed offset: the old centring wrote outside the scratch frame. */
    PwVideoOutLayout layout;
    int status=pw_videoout_layout(view->width,view->height,WIDTH,HEIGHT,80u,3u,&layout);
    if(status!=PW_OK)return status;
    const uint32_t scale=layout.scale;
    /* The background shows only where the frame does not cover the screen. */
    if(layout.source_width*scale<WIDTH || layout.source_height*scale<HEIGHT)
        for(uint32_t y=0;y<HEIGHT;y++)for(uint32_t x=0;x<WIDTH;x++)
            output[pw_videoout_tiles_index(&tiles,x,y)]=background;
    for(uint32_t sy=0;sy<layout.source_height;sy++) {
        const uint32_t *source=(const uint32_t *)(view->pixels+(size_t)sy*view->stride);
        for(uint32_t yy=0;yy<scale;yy++) {
            const uint32_t y=layout.top+sy*scale+yy,row_base=tiles.row_base[y],
                           row_swizzle=tiles.row_swizzle[y];
            for(uint32_t sx=0;sx<layout.source_width;sx++)for(uint32_t xx=0;xx<scale;xx++) {
                const uint32_t x=layout.left+sx*scale+xx;
                output[tiles.column_base[x]+row_base+(tiles.column_swizzle[x]^row_swizzle)]=
                    source[sx]|0xff000000u;
            }
        }
    }
    status=pw_agc_ps5_copy_flip(&video->agc,video->handle,(int)index,output,
                                    scanout,(uint32_t)video->frame_bytes,video->flips+1);
    if(status!=PW_OK)return status;
    (void)sceVideoOutWaitVblank(video->handle);video->index=index;video->flips++;return PW_OK;
}
