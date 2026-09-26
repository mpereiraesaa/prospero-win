/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Host stand-in for ntdll.prx's present sink and input queue, preloaded
 * into a host Wine built with WINE_PS5_USER_DRIVER (Wine patch 0400) by
 * tools/test_wine_ps5_driver.sh. Every presented frame is counted and the
 * latest one is written to $PW_FRAME_DUMP as a binary PPM. Once
 * $PW_INPUT_AFTER frames have been shown, the text in $PW_INPUT_TEXT
 * (A-Z, 0-9 and space) is typed once as virtual-key presses, and a left
 * click is made at $PW_INPUT_CLICK ("x,y" on the desktop). The frame count
 * and size are printed to stderr at exit. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

struct input { uint32_t type,code; int32_t x,y; uint32_t down; };  /* PwWineInput */

static unsigned long frames;
static uint32_t last_width,last_height;
static const char *text;
static size_t typed;       /* events handed out: two per character */
static int clicked;        /* click events handed out: move, down, up */

int pw_wine_present(const void *bgra,uint32_t width,uint32_t height,uint32_t stride);
int pw_wine_next_input(struct input *event);
int pw_wine_input_fd(void);

/* Always readable: the scripted input is ready as soon as it is due. */
int pw_wine_input_fd(void)
{
    static int fds[2]={-1,-1};
    if(fds[0]==-1 && !pipe(fds)) {
        fcntl(fds[0],F_SETFL,O_NONBLOCK);fcntl(fds[1],F_SETFL,O_NONBLOCK);
    }
    if(fds[1]!=-1 && write(fds[1],"x",1)<0){}
    return fds[0];
}

int pw_wine_present(const void *bgra,uint32_t width,uint32_t height,uint32_t stride)
{
    const char *path=getenv("PW_FRAME_DUMP");
    frames++;last_width=width;last_height=height;
    if(!path)return 0;
    char temp[4096];
    snprintf(temp,sizeof(temp),"%s.tmp",path);
    FILE *f=fopen(temp,"wb");
    if(!f)return 0;
    fprintf(f,"P6\n%u %u\n255\n",width,height);
    for(uint32_t y=0;y<height;y++) {
        const uint8_t *row=(const uint8_t *)bgra+(size_t)y*stride;
        for(uint32_t x=0;x<width;x++) {
            uint8_t rgb[3]={row[x*4+2],row[x*4+1],row[x*4]};
            fwrite(rgb,1,3,f);
        }
    }
    fclose(f);
    rename(temp,path);
    return 0;
}

int pw_wine_next_input(struct input *event)
{
    const char *after=getenv("PW_INPUT_AFTER"),*click=getenv("PW_INPUT_CLICK");
    int x,y;
    if(frames<(unsigned long)(after?atol(after):1))return 0;
    if(click && clicked<3 && sscanf(click,"%d,%d",&x,&y)==2) {
        *event=clicked==0?(struct input){2,0,x,y,0}:(struct input){3,0,0,0,clicked==1};
        clicked++;
        return 1;
    }
    if(!text)text=getenv("PW_INPUT_TEXT");
    if(!text || typed>=2*strlen(text))return 0;
    char c=text[typed/2];
    uint32_t vk=c==' '?0x20:(c>='a' && c<='z')?(uint32_t)(c-'a'+'A'):(uint32_t)c;
    *event=(struct input){1,vk,0,0,typed%2==0};
    typed++;
    return 1;
}

__attribute__((destructor)) static void report(void)
{
    fprintf(stderr,"ps5 driver sink: frames=%lu size=%ux%u typed=%zu\n",frames,last_width,last_height,typed/2);
}
