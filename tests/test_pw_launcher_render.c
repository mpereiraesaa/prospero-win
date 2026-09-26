/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_launcher_render.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { W=1920,H=1080,STRIDE=W*4+16 };
static uint8_t pixels[STRIDE*H];

static uint32_t hash(void)
{
    uint32_t h=2166136261u;
    for(uint32_t y=0;y<H;y++)for(uint32_t i=0;i<W*4u;i++){h^=pixels[y*STRIDE+i];h*=16777619u;}
    return h;
}
static uint32_t at(uint32_t x,uint32_t y)
{
    const uint8_t *p=pixels+(size_t)y*STRIDE+x*4u;
    assert(p[3]==0xff);
    return (uint32_t)p[2]<<16|(uint32_t)p[1]<<8|p[0];
}
static uint32_t render(const PwLauncherScene *scene)
{
    memset(pixels,0x5a,sizeof(pixels));
    const PwPresentTarget target={pixels,W,H,STRIDE,sizeof(pixels)};
    assert(pw_launcher_render(scene,&target)==PW_OK);
    for(uint32_t y=0;y<H;y++)for(uint32_t i=W*4u;i<STRIDE;i++)assert(pixels[y*STRIDE+i]==0x5a);
    return hash();
}

int main(void)
{
    const PwLauncherItem items[]={
        {"Space Cadet Pinball","pe32  gdi  prospero-win-direct",1},
        {"Paint","pe32  gdi  wine-wow64",0},
        {"Minesweeper",NULL,1},
    };
    const PwLauncherScene library={items,3,0,"Ready"};
    const PwLauncherScene unavailable={items,3,1,"Pinball closed normally"};
    const PwLauncherScene empty={NULL,0,PW_LAUNCHER_RENDER_NONE,NULL};
    uint32_t hashes[3]={render(&library),render(&unavailable),render(&empty)};
    /* Golden frames: a visual change must update these deliberately, after
     * looking at the new rendering. */
    const uint32_t golden[3]={0xbd18e38fu,0x2ee60186u,0x59bd4231u};
    if(memcmp(hashes,golden,sizeof(golden)))
        printf("launcher render hashes 0x%08x 0x%08x 0x%08x\n",hashes[0],hashes[1],hashes[2]);
    assert(!memcmp(hashes,golden,sizeof(golden)));

    /* Semantic probes on the library scene. */
    render(&library);
    assert(at(10,1)==0x8cbcff);                     /* title bar highlight */
    assert((at(10,60)&0xff)>0xc0);                  /* blue title bar */
    assert(at(0,1079)==at(120,1079) && (at(120,1079)>>8&0xff)>0x80); /* green button */
    /* Tiles start at x=140 (three 520-pixel tiles, 40 apart); a selected
     * tile carries an 8-pixel orange frame. */
    uint32_t selected_edge=at(135,300),plain_edge=at(695,300);
    assert(selected_edge>>16>0xe0 && (selected_edge&0xff)<0x50);
    assert(plain_edge!=selected_edge);
    /* Available bodies are pale blue, unavailable ones neutral grey. */
    uint32_t available_body=at(620,420),unavailable_body=at(1200,420);
    assert((available_body&0xff)>(available_body>>16&0xff));
    uint32_t ur=unavailable_body>>16&0xff,ug=unavailable_body>>8&0xff,ub=unavailable_body&0xff;
    uint32_t ar=available_body>>16&0xff,ab=available_body&0xff;
    assert(ub>=ur && ub-ur<0x10 && ug>=ur && ug-ur<0x08);   /* near-neutral */
    assert(ab-ar>ub-ur);                                     /* bluer when available */
    /* Moving the selection moves the highlight. */
    render(&unavailable);
    assert(at(135,300)!=selected_edge && at(695,300)==selected_edge);

    /* A second page is shown for a selection past the first six tiles. */
    PwLauncherItem many[8];
    for(unsigned i=0;i<8;i++)many[i]=(PwLauncherItem){i<6?"First":"Second",NULL,1};
    const PwLauncherScene first={many,8,5,NULL},second={many,8,6,NULL};
    assert(render(&first)!=render(&second));
    render(&second);assert(at(135,300)==selected_edge);

    const PwPresentTarget target={pixels,W,H,STRIDE,sizeof(pixels)};
    PwPresentTarget small=target;small.width=1280;
    assert(pw_launcher_render(&library,&small)==PW_ERR_MALFORMED);
    small=target;small.bytes=(uint64_t)STRIDE*H-1;
    assert(pw_launcher_render(&library,&small)==PW_ERR_MALFORMED);
    const PwLauncherScene bad_selection={items,3,3,NULL};
    assert(pw_launcher_render(&bad_selection,&target)==PW_ERR_PRECONDITION);
    const PwLauncherItem untitled[]={{NULL,NULL,1}};
    const PwLauncherScene bad_item={untitled,1,0,NULL};
    assert(pw_launcher_render(&bad_item,&target)==PW_ERR_PRECONDITION);
    assert(pw_launcher_render(NULL,&target)==PW_ERR_PRECONDITION);
    return 0;
}
