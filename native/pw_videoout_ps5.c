/* SPDX-License-Identifier: LGPL-2.1-or-later AND Zlib */
/*
 * A small standalone scalar presenter for the PS5's VideoOut. Its
 * pixel-address permutation is adapted from the tilemap code of SDL's PS5
 * video backend, which carries the notice below; the adaptation is altered
 * from the original. The rest of the file is prospero-win's own code under
 * LGPL-2.1-or-later.
 *
 * Copyright (C) 2026 John Törnblom <john.tornblom@gmail.com>
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */
#include "pw_videoout_ps5.h"
#include "pw_videoout_tile.h"
#include "pw_videoout_layout.h"
#include <string.h>

enum { WIDTH=PW_VIDEOOUT_PS5_WIDTH,HEIGHT=PW_VIDEOOUT_PS5_HEIGHT,FRAME_BYTES=PW_VIDEOOUT_PS5_FRAME_BYTES,
       MEMORY_BYTES=0x3000000 };
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

/* Column and row parts of the tiled index, filled once at open, and the
 * scaler's rows. */
static PwVideoOutTiles tiles;
static PwVideoOutScaleRows scale_rows;

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
    sceVideoOutSetBufferAttribute2(&attribute,PW_VIDEOOUT_PS5_PIXEL_FORMAT,0,WIDTH,HEIGHT,0,0,0);
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
/* The tiled image is drawn in the third frame of video memory, then copied
 * to the scanout buffer not on screen and flipped to at the vblank. */
static uint32_t *draw_frame(PwVideoOutPs5 *video)
{
    return (uint32_t *)((uint8_t *)video->memory+2u*video->frame_bytes);
}
static int flip(PwVideoOutPs5 *video)
{
    unsigned index=video->index^1u;
    void *scanout=(uint8_t *)video->memory+(size_t)index*video->frame_bytes;
    int status=pw_agc_ps5_copy_flip(&video->agc,video->handle,(int)index,draw_frame(video),
                                    scanout,(uint32_t)video->frame_bytes,video->flips+1);
    if(status!=PW_OK)return status;
    (void)sceVideoOutWaitVblank(video->handle);video->index=index;video->flips++;return PW_OK;
}
int pw_videoout_ps5_present_scaled(PwVideoOutPs5 *video,const PwPresentFrame *frame,int mode,
                                   uint32_t background)
{
    if(!video || !video->opened)return PW_ERR_PRECONDITION;
    PwPresentPlacement placement;
    int status=pw_present_scale_placement(frame,mode,WIDTH,HEIGHT,&placement);
    if(status!=PW_OK)return status;
    pw_videoout_tiles_scale(&tiles,&scale_rows,frame,&placement,background,draw_frame(video));
    return flip(video);
}
int pw_videoout_ps5_draw_scaled(const PwPresentFrame *frame,int mode,uint32_t background,
                                uint32_t *out)
{
    if(!frame || !out)return PW_ERR_PRECONDITION;
    PwPresentPlacement placement;
    int status=pw_present_scale_placement(frame,mode,WIDTH,HEIGHT,&placement);
    if(status!=PW_OK)return status;
    pw_videoout_tiles_init(&tiles);
    pw_videoout_tiles_scale_to(&tiles,&scale_rows,frame,&placement,background,1,out);
    return PW_OK;
}
int pw_videoout_ps5_present(PwVideoOutPs5 *video,const PwPresentView *view)
{
    if(!video || !video->opened || !view || !view->pixels || !view->width || !view->height)
        return PW_ERR_PRECONDITION;
    uint32_t *output=draw_frame(video);
    uint32_t background=0xff181010u;
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
                    pw_videoout_rgbx(source[sx]);
            }
        }
    }
    return flip(video);
}
