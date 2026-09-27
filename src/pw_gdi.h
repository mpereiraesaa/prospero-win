/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_GDI_H
#define PW_GDI_H
#include "../include/prospero_win.h"
#include "pw_present.h"

enum {
    PW_GDI_HANDLE_FIRST=0x00020000u,
    PW_GDI_STOCK_BITMAP=0x7f000001u,
    PW_GDI_STOCK_OBJECT_BASE=0x7f010000u,
    PW_GDI_ROP_BLACKNESS=0x00000042u,
    PW_GDI_ROP_SRCCOPY=0x00cc0020u,
    PW_GDI_NO_SURFACE=UINT32_MAX,
    PW_GDI_CAP_NUMCOLORS=24,
    PW_GDI_CAP_RASTERCAPS=38,
    PW_GDI_RC_BITBLT=1,
    PW_GDI_PALETTE_CAPACITY=16,
    PW_GDI_PALETTE_ENTRIES=256
};
typedef enum PwGdiDcKind { PW_GDI_WINDOW_DC=1,PW_GDI_MEMORY_DC=2 } PwGdiDcKind;
typedef enum PwGdiSurfaceKind { PW_GDI_TARGET_SURFACE=1,PW_GDI_BITMAP=2 } PwGdiSurfaceKind;
typedef struct PwGdiDc {
    uint32_t handle,owner_window,compatible_dc,surface,selected_bitmap,selected_palette,layout;
    PwGdiDcKind kind;
    unsigned used;
} PwGdiDc;
typedef struct PwGdiSurface {
    uint32_t handle,target,width,height,stride,offset,bytes,selected_by;
    PwGdiSurfaceKind kind;
    unsigned used;
} PwGdiSurface;
typedef struct PwGdiPalette {
    uint32_t handle,selected_by;
    uint16_t version,count;
    uint8_t entries[PW_GDI_PALETTE_ENTRIES][4];
    unsigned used;
} PwGdiPalette;
typedef struct PwGdi {
    PwGdiDc *dcs;
    PwGdiSurface *surfaces;
    uint8_t *pixels;
    uint32_t dc_capacity,surface_capacity,pixel_capacity,next_handle;
    uint32_t system_palette_use;
    /* Read-only diagnostics for the latest accepted StretchDIBits call.  The
     * addresses are guest PE32 addresses and never imply ownership. */
    uint64_t stretch_calls;
    uint32_t stretch_owner,stretch_bits,stretch_info,stretch_dib_width,stretch_dib_height;
    int32_t stretch_x,stretch_y,stretch_width,stretch_height;
    int32_t stretch_source_x,stretch_source_y,stretch_source_width,stretch_source_height;
    PwGdiPalette palettes[PW_GDI_PALETTE_CAPACITY];
} PwGdi;
typedef struct PwGdiCounts {
    uint32_t dcs,window_dcs,memory_dcs,surfaces,target_surfaces,bitmaps,palettes;
    uint64_t pixel_bytes;
} PwGdiCounts;
typedef struct PwGdiBitmapInfo {
    uint32_t width,height,stride,planes,bits_per_pixel;
} PwGdiBitmapInfo;
typedef PwPresentView PwGdiTargetView;

int pw_gdi_init(PwGdi *,PwGdiDc *,uint32_t,PwGdiSurface *,uint32_t,
                uint8_t *,uint32_t);
int pw_gdi_get_dc(PwGdi *,uint32_t owner_window,uint32_t width,uint32_t height,
                  uint32_t *dc);
int pw_gdi_release_dc(PwGdi *,uint32_t owner_window,uint32_t dc);
int pw_gdi_create_compatible_dc(PwGdi *,uint32_t source_dc,uint32_t *dc);
int pw_gdi_create_compatible_bitmap(PwGdi *,uint32_t source_dc,uint32_t width,
                                    uint32_t height,uint32_t *bitmap);
int pw_gdi_create_dib_bitmap(PwGdi *,const uint8_t *dib,uint32_t dib_bytes,
                             uint32_t *bitmap);
int pw_gdi_create_palette(PwGdi *,uint16_t version,uint16_t count,
                          const uint8_t entries[][4],uint32_t *palette);
int pw_gdi_set_palette_entries(PwGdi *,uint32_t palette,uint32_t start,
                               uint32_t count,const uint8_t entries[][4],uint32_t *written);
int pw_gdi_resize_palette(PwGdi *,uint32_t palette,uint32_t count);
int pw_gdi_stock_object(uint32_t index,uint32_t *object);
int pw_gdi_select_bitmap(PwGdi *,uint32_t dc,uint32_t bitmap,uint32_t *previous);
int pw_gdi_delete_dc(PwGdi *,uint32_t dc);
int pw_gdi_delete_object(PwGdi *,uint32_t object);
int pw_gdi_get_layout(PwGdi *,uint32_t dc,uint32_t *layout);
int pw_gdi_set_layout(PwGdi *,uint32_t dc,uint32_t layout,uint32_t *previous);
int pw_gdi_get_device_caps(PwGdi *,uint32_t dc,uint32_t index,uint32_t *value);
int pw_gdi_bitmap_info(PwGdi *,uint32_t bitmap,PwGdiBitmapInfo *);
int pw_gdi_resize_target(PwGdi *,uint32_t owner_window,uint32_t width,uint32_t height);
int pw_gdi_destroy_target(PwGdi *,uint32_t owner_window);
int pw_gdi_target_view(const PwGdi *,uint32_t owner_window,PwGdiTargetView *);
int pw_gdi_select_palette(PwGdi *,uint32_t dc,uint32_t palette,uint32_t background,
                          uint32_t *previous);
int pw_gdi_realize_palette(PwGdi *,uint32_t dc,uint32_t *changed);
int pw_gdi_set_system_palette_use(PwGdi *,uint32_t dc,uint32_t use,uint32_t *previous);
int pw_gdi_get_system_palette_entries(PwGdi *,uint32_t dc,uint32_t start,uint32_t count,
                                      uint8_t entries[][4],uint32_t *copied);
int pw_gdi_bitblt(PwGdi *,uint32_t destination_dc,int32_t x,int32_t y,
                  uint32_t width,uint32_t height,uint32_t source_dc,
                  int32_t source_x,int32_t source_y,uint32_t rop);
int pw_gdi_stretch_dibits(PwGdi *,uint32_t destination_dc,
                          int32_t x,int32_t y,int32_t width,int32_t height,
                          int32_t source_x,int32_t source_y,
                          int32_t source_width,int32_t source_height,
                          const uint8_t *bits,uint32_t bits_bytes,
                          const uint8_t *info,uint32_t info_bytes,
                          uint32_t usage,uint32_t rop,int32_t *scan_lines);
int pw_gdi_counts(const PwGdi *,PwGdiCounts *);
int pw_gdi_validate(const PwGdi *);
int pw_gdi_reset(PwGdi *);

#endif
