/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_launcher_render.h"
#include <string.h>

enum { W=PW_LAUNCHER_RENDER_WIDTH,H=PW_LAUNCHER_RENDER_HEIGHT,
       BAR=96,TASKBAR_Y=996,TILE_W=520,TILE_H=240,TILE_GAP=40,
       TILES_X=(W-3*TILE_W-2*TILE_GAP)/2,TILES_Y=190 };

typedef struct Glyph { char c;uint8_t rows[7]; } Glyph;
/* Original 5x7 dot-matrix glyphs; bit 4 is the leftmost column. */
static const Glyph glyphs[]={
    {' ',{0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
    {'!',{0x04,0x04,0x04,0x04,0x04,0x00,0x04}},
    {'\'',{0x04,0x04,0x08,0x00,0x00,0x00,0x00}},
    {'(',{0x02,0x04,0x08,0x08,0x08,0x04,0x02}},
    {')',{0x08,0x04,0x02,0x02,0x02,0x04,0x08}},
    {'+',{0x00,0x04,0x04,0x1f,0x04,0x04,0x00}},
    {',',{0x00,0x00,0x00,0x00,0x0c,0x04,0x08}},
    {'-',{0x00,0x00,0x00,0x1f,0x00,0x00,0x00}},
    {'.',{0x00,0x00,0x00,0x00,0x00,0x0c,0x0c}},
    {'/',{0x00,0x01,0x02,0x04,0x08,0x10,0x00}},
    {'0',{0x0e,0x11,0x13,0x15,0x19,0x11,0x0e}},
    {'1',{0x04,0x0c,0x04,0x04,0x04,0x04,0x0e}},
    {'2',{0x0e,0x11,0x01,0x02,0x04,0x08,0x1f}},
    {'3',{0x1f,0x02,0x04,0x02,0x01,0x11,0x0e}},
    {'4',{0x02,0x06,0x0a,0x12,0x1f,0x02,0x02}},
    {'5',{0x1f,0x10,0x1e,0x01,0x01,0x11,0x0e}},
    {'6',{0x06,0x08,0x10,0x1e,0x11,0x11,0x0e}},
    {'7',{0x1f,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8',{0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e}},
    {'9',{0x0e,0x11,0x11,0x0f,0x01,0x02,0x0c}},
    {':',{0x00,0x0c,0x0c,0x00,0x0c,0x0c,0x00}},
    {'=',{0x00,0x00,0x1f,0x00,0x1f,0x00,0x00}},
    {'>',{0x08,0x04,0x02,0x01,0x02,0x04,0x08}},
    {'?',{0x0e,0x11,0x01,0x02,0x04,0x00,0x04}},
    {'A',{0x0e,0x11,0x11,0x1f,0x11,0x11,0x11}},
    {'B',{0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e}},
    {'C',{0x0e,0x11,0x10,0x10,0x10,0x11,0x0e}},
    {'D',{0x1e,0x11,0x11,0x11,0x11,0x11,0x1e}},
    {'E',{0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f}},
    {'F',{0x1f,0x10,0x10,0x1e,0x10,0x10,0x10}},
    {'G',{0x0e,0x11,0x10,0x17,0x11,0x11,0x0f}},
    {'H',{0x11,0x11,0x11,0x1f,0x11,0x11,0x11}},
    {'I',{0x0e,0x04,0x04,0x04,0x04,0x04,0x0e}},
    {'J',{0x07,0x02,0x02,0x02,0x02,0x12,0x0c}},
    {'K',{0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
    {'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1f}},
    {'M',{0x11,0x1b,0x15,0x15,0x11,0x11,0x11}},
    {'N',{0x11,0x11,0x19,0x15,0x13,0x11,0x11}},
    {'O',{0x0e,0x11,0x11,0x11,0x11,0x11,0x0e}},
    {'P',{0x1e,0x11,0x11,0x1e,0x10,0x10,0x10}},
    {'Q',{0x0e,0x11,0x11,0x11,0x15,0x12,0x0d}},
    {'R',{0x1e,0x11,0x11,0x1e,0x14,0x12,0x11}},
    {'S',{0x0f,0x10,0x10,0x0e,0x01,0x01,0x1e}},
    {'T',{0x1f,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U',{0x11,0x11,0x11,0x11,0x11,0x11,0x0e}},
    {'V',{0x11,0x11,0x11,0x11,0x11,0x0a,0x04}},
    {'W',{0x11,0x11,0x11,0x15,0x15,0x15,0x0a}},
    {'X',{0x11,0x11,0x0a,0x04,0x0a,0x11,0x11}},
    {'Y',{0x11,0x11,0x0a,0x04,0x04,0x04,0x04}},
    {'Z',{0x1f,0x01,0x02,0x04,0x08,0x10,0x1f}},
    {'_',{0x00,0x00,0x00,0x00,0x00,0x00,0x1f}},
};

typedef struct Canvas { uint8_t *pixels;uint32_t stride; } Canvas;

static uint32_t rgb(uint32_t r,uint32_t g,uint32_t b){return r<<16|g<<8|b;}
static uint32_t mix(uint32_t a,uint32_t b,uint32_t n,uint32_t d)
{
    uint32_t out=0;
    for(unsigned shift=0;shift<24;shift+=8) {
        uint32_t x=(a>>shift)&0xffu,y=(b>>shift)&0xffu;
        out|=((x*(d-n)+y*n)/d)<<shift;
    }
    return out;
}
static void put(const Canvas *c,int32_t x,int32_t y,uint32_t color)
{
    if(x<0 || y<0 || x>=W || y>=H)return;
    uint8_t *p=c->pixels+(size_t)y*c->stride+(size_t)x*4u;
    p[0]=(uint8_t)color;p[1]=(uint8_t)(color>>8);p[2]=(uint8_t)(color>>16);p[3]=0xff;
}
static void fill(const Canvas *c,int32_t x,int32_t y,int32_t w,int32_t h,uint32_t color)
{
    for(int32_t j=0;j<h;j++)for(int32_t i=0;i<w;i++)put(c,x+i,y+j,color);
}
/* Vertical ramp from top to bottom colour. */
static void ramp(const Canvas *c,int32_t x,int32_t y,int32_t w,int32_t h,
                 uint32_t top,uint32_t bottom)
{
    for(int32_t j=0;j<h;j++)
        fill(c,x,y+j,w,1,mix(top,bottom,(uint32_t)j,h>1?(uint32_t)(h-1):1u));
}
/* A pixel lies outside a rounded corner of radius r. */
static int cut(int32_t i,int32_t j,int32_t w,int32_t h,int32_t r)
{
    int32_t dx=i<r?r-1-i:i>=w-r?i-(w-r):-1,dy=j<r?r-1-j:j>=h-r?j-(h-r):-1;
    return dx>=0 && dy>=0 && dx*dx+dy*dy>=r*r;
}
static void rounded(const Canvas *c,int32_t x,int32_t y,int32_t w,int32_t h,int32_t r,
                    uint32_t top,uint32_t bottom)
{
    for(int32_t j=0;j<h;j++) {
        uint32_t color=mix(top,bottom,(uint32_t)j,h>1?(uint32_t)(h-1):1u);
        for(int32_t i=0;i<w;i++)if(!cut(i,j,w,h,r))put(c,x+i,y+j,color);
    }
}
static const Glyph *glyph(char ch)
{
    if(ch>='a' && ch<='z')ch=(char)(ch-'a'+'A');
    const Glyph *unknown=NULL;
    for(size_t i=0;i<sizeof(glyphs)/sizeof(glyphs[0]);i++) {
        if(glyphs[i].c==ch)return &glyphs[i];
        if(glyphs[i].c=='?')unknown=&glyphs[i];
    }
    return unknown;
}
static int32_t text_width(const char *text,int32_t scale)
{
    size_t n=text?strlen(text):0;return n?(int32_t)(n*6u-1u)*scale:0;
}
/* Draws text clipped to max_width pixels; returns the width used. */
static int32_t text(const Canvas *c,int32_t x,int32_t y,const char *s,int32_t scale,
                    uint32_t color,int32_t max_width)
{
    int32_t used=0;
    for(;s && *s;s++) {
        if(used+5*scale>max_width)break;
        const Glyph *g=glyph(*s);
        for(int32_t row=0;row<7;row++)for(int32_t col=0;col<5;col++)
            if(g->rows[row]&(0x10u>>col))
                fill(c,x+used+col*scale,y+row*scale,scale,scale,color);
        used+=6*scale;
    }
    return used;
}
/* The largest scale from preferred down to minimum at which text fits;
 * text that does not fit even at the minimum is clipped by text(). */
static int32_t fit(const char *s,int32_t preferred,int32_t minimum,int32_t max_width)
{
    int32_t scale=preferred;
    while(scale>minimum && text_width(s,scale)>max_width)scale--;
    return scale;
}
static void background(const Canvas *c)
{
    /* Sky ramp with a rolling hill whose crest is a parabola. */
    for(int32_t y=0;y<H;y++) {
        uint32_t sky=mix(rgb(0x1f,0x5c,0xc4),rgb(0xa6,0xcc,0xf4),(uint32_t)y,H-1);
        for(int32_t x=0;x<W;x++) {
            int32_t d=x-W*3/5;
            int32_t crest=700+(int32_t)((int64_t)d*d*220/((int64_t)W*W));
            put(c,x,y,y<crest?sky:mix(rgb(0x62,0xba,0x46),rgb(0x2a,0x78,0x22),
                (uint32_t)(y-crest),(uint32_t)(H-crest)));
        }
    }
}
static void title_bar(const Canvas *c)
{
    ramp(c,0,0,W,BAR,rgb(0x3a,0x86,0xf2),rgb(0x0a,0x44,0xc0));
    fill(c,0,0,W,3,rgb(0x8c,0xbc,0xff));
    fill(c,0,BAR-3,W,3,rgb(0x06,0x2e,0x8a));
    text(c,48,27,"prospero-win",6,rgb(0xff,0xff,0xff),W);
    text(c,W-48-text_width("Library",5),31,"Library",5,rgb(0xdc,0xea,0xff),W);
}
static void tile(const Canvas *c,int32_t x,int32_t y,const PwLauncherItem *item,int selected)
{
    if(selected)rounded(c,x-8,y-8,TILE_W+16,TILE_H+16,20,rgb(0xff,0xc2,0x4a),rgb(0xe8,0x8a,0x10));
    uint32_t edge=item->available?rgb(0x6e,0x96,0xd8):rgb(0x98,0x9c,0xa6);
    rounded(c,x,y,TILE_W,TILE_H,14,edge,edge);
    uint32_t top=item->available?rgb(0xff,0xff,0xff):rgb(0xe6,0xe8,0xec);
    uint32_t bottom=item->available?rgb(0xdc,0xe8,0xfa):rgb(0xc8,0xcc,0xd4);
    rounded(c,x+4,y+4,TILE_W-8,TILE_H-8,11,top,bottom);
    /* Header strip carrying the title. */
    ramp(c,x+4,y+16,TILE_W-8,66,item->available?rgb(0x4a,0x8e,0xee):rgb(0x9a,0xa0,0xac),
         item->available?rgb(0x22,0x5e,0xcc):rgb(0x74,0x7a,0x86));
    int32_t title_scale=fit(item->title,5,3,TILE_W-48);
    text(c,x+24,y+49-7*title_scale/2,item->title,title_scale,rgb(0xff,0xff,0xff),TILE_W-48);
    if(item->detail)text(c,x+24,y+108,item->detail,fit(item->detail,3,2,TILE_W-48),
                         rgb(0x2c,0x3a,0x5c),TILE_W-48);
    if(item->available) {
        rounded(c,x+24,y+TILE_H-66,168,44,22,rgb(0x5c,0xc4,0x4c),rgb(0x2e,0x8c,0x28));
        text(c,x+24+(168-text_width("Ready",4))/2,y+TILE_H-58,"Ready",4,rgb(0xff,0xff,0xff),168);
    } else {
        text(c,x+24,y+TILE_H-56,"Not available yet",4,rgb(0xb4,0x3c,0x1c),TILE_W-48);
    }
}
static void taskbar(const Canvas *c,const char *status)
{
    ramp(c,0,TASKBAR_Y,W,H-TASKBAR_Y,rgb(0x3c,0x80,0xf0),rgb(0x14,0x4a,0xc4));
    fill(c,0,TASKBAR_Y,W,3,rgb(0x92,0xbe,0xff));
    /* Green start-like button with a rounded right edge. */
    int32_t bw=236,bh=H-TASKBAR_Y-6;
    for(int32_t j=0;j<bh;j++) {
        uint32_t color=mix(rgb(0x62,0xc0,0x52),rgb(0x2a,0x88,0x26),(uint32_t)j,(uint32_t)(bh-1));
        for(int32_t i=0;i<bw;i++)if(!cut(i+30,j,bw+30,bh,30))put(c,i,TASKBAR_Y+6+j,color);
    }
    text(c,(bw-text_width("Play",6))/2,TASKBAR_Y+6+(bh-42)/2,"Play",6,rgb(0xff,0xff,0xff),bw);
    int32_t hints=text(c,bw+48,TASKBAR_Y+30,"Cross: launch   Options+Create (hold): close",3,
                       rgb(0xff,0xff,0xff),W-bw-96);
    if(status && *status) {
        int32_t tray=W-(bw+48+hints+48);
        int32_t width=text_width(status,3);if(width>tray-48)width=tray-48;
        if(width>0) {
            fill(c,W-width-48,TASKBAR_Y+3,width+48,H-TASKBAR_Y-3,rgb(0x0e,0x8c,0xea));
            text(c,W-width-24,TASKBAR_Y+30,status,3,rgb(0xff,0xff,0xff),width);
        }
    }
}

int pw_launcher_render(const PwLauncherScene *scene,const PwPresentTarget *target)
{
    if(!scene || !target || !target->pixels || (scene->count && !scene->items))
        return PW_ERR_PRECONDITION;
    if(target->width!=W || target->height!=H || target->stride<(uint32_t)W*4u ||
       target->stride%4u || (uint64_t)target->stride*H>target->bytes)return PW_ERR_MALFORMED;
    if(scene->selected!=PW_LAUNCHER_RENDER_NONE && scene->selected>=scene->count)
        return PW_ERR_PRECONDITION;
    for(uint32_t i=0;i<scene->count;i++)if(!scene->items[i].title)return PW_ERR_PRECONDITION;
    const Canvas canvas={target->pixels,target->stride};
    background(&canvas);title_bar(&canvas);
    if(!scene->count) {
        const char *empty="No application profiles found";
        text(&canvas,(W-text_width(empty,5))/2,480,empty,5,rgb(0xff,0xff,0xff),W);
    }
    uint32_t first=scene->selected==PW_LAUNCHER_RENDER_NONE?0:
        scene->selected/PW_LAUNCHER_RENDER_PAGE*PW_LAUNCHER_RENDER_PAGE;
    for(uint32_t i=first;i<scene->count && i<first+PW_LAUNCHER_RENDER_PAGE;i++) {
        uint32_t slot=i-first;
        int32_t x=TILES_X+(int32_t)(slot%3u)*(TILE_W+TILE_GAP);
        int32_t y=TILES_Y+(int32_t)(slot/3u)*(TILE_H+TILE_GAP+24);
        tile(&canvas,x,y,&scene->items[i],i==scene->selected);
    }
    if(scene->count>PW_LAUNCHER_RENDER_PAGE) {
        char page[]="Page 0/0";
        page[5]=(char)('1'+first/PW_LAUNCHER_RENDER_PAGE);
        page[7]=(char)('0'+(scene->count+PW_LAUNCHER_RENDER_PAGE-1)/PW_LAUNCHER_RENDER_PAGE);
        text(&canvas,W-48-text_width(page,3),TILES_Y+2*(TILE_H+TILE_GAP+24),page,3,
             rgb(0xff,0xff,0xff),W);
    }
    taskbar(&canvas,scene->status);
    return PW_OK;
}
