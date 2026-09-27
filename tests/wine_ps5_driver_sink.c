/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Host stand-in for ntdll.prx's present sink and input queue, preloaded
 * into a host Wine built with WINE_PS5_USER_DRIVER (Wine patch 0400) by
 * tools/test_wine_ps5_driver.sh. Every presented frame is counted and the
 * latest one is written to $PW_FRAME_DUMP as a binary PPM. Once
 * $PW_INPUT_AFTER frames have been shown, the text in $PW_INPUT_TEXT
 * (A-Z, 0-9 and space) is typed once as virtual-key presses, and a left
 * click is made at $PW_INPUT_CLICK ("x,y" on the desktop). The pointer then
 * visits each point of $PW_INPUT_MOVES ("x,y;x,y;..."), one every 300 ms so
 * that the game can set its cursor. When $PW_FRAME_DIR is set it receives
 * every frame as NNNN.ppm, which shows where the driver drew the cursor.
 * When $PW_SINK_PROCESS is set, only a process whose command line contains
 * it presents frames and takes input, because explorer and services load
 * the driver too. The frame count and size are printed to stderr at exit. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

struct input { uint32_t type,code; int32_t x,y; uint32_t down; };  /* PwWineInput */

static unsigned long frames;
static uint32_t last_width,last_height;
static const char *text;
static size_t typed;       /* events handed out: two per character */
static int clicked;        /* click events handed out: move, down, up */
static int moved;          /* points of PW_INPUT_MOVES visited */
static uint64_t moved_ms;  /* when the last one was */
static int fds[2]={-1,-1};

int pw_wine_present(const void *bgra,uint32_t width,uint32_t height,uint32_t stride);
int pw_wine_next_input(struct input *event);
int pw_wine_input_fd(void);

static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000u+(uint64_t)t.tv_nsec/1000000u;
}

/* Whether this process is the one the sink serves. */
static int is_target(void)
{
    static int known=-1;
    const char *name=getenv("PW_SINK_PROCESS");
    if(known<0) {
        char line[4096]={0};
        FILE *f=fopen("/proc/self/cmdline","rb");
        size_t n=f?fread(line,1,sizeof(line)-1,f):0;
        if(f)fclose(f);
        for(size_t i=0;i<n;i++)if(!line[i])line[i]=' ';
        known=!name || strstr(line,name)!=NULL;
    }
    return known;
}

/* Paced moves fall due later: wake the driver's input wait meanwhile. */
static void *wake(void *arg)
{
    (void)arg;
    for(;;){usleep(100000);if(write(fds[1],"x",1)<0){}}
    return NULL;
}

/* Always readable: the scripted input is ready as soon as it is due. */
int pw_wine_input_fd(void)
{
    if(fds[0]==-1 && !pipe(fds)) {
        pthread_t thread;
        fcntl(fds[0],F_SETFL,O_NONBLOCK);fcntl(fds[1],F_SETFL,O_NONBLOCK);
        if(getenv("PW_INPUT_MOVES") && is_target() && !pthread_create(&thread,NULL,wake,NULL))
            pthread_detach(thread);
    }
    if(fds[1]!=-1 && write(fds[1],"x",1)<0){}
    return fds[0];
}

static void write_ppm(const char *path,const void *bgra,uint32_t width,uint32_t height,uint32_t stride)
{
    FILE *f=fopen(path,"wb");
    if(!f)return;
    fprintf(f,"P6\n%u %u\n255\n",width,height);
    for(uint32_t y=0;y<height;y++) {
        const uint8_t *row=(const uint8_t *)bgra+(size_t)y*stride;
        for(uint32_t x=0;x<width;x++) {
            uint8_t rgb[3]={row[x*4+2],row[x*4+1],row[x*4]};
            fwrite(rgb,1,3,f);
        }
    }
    fclose(f);
}

int pw_wine_present(const void *bgra,uint32_t width,uint32_t height,uint32_t stride)
{
    const char *path=getenv("PW_FRAME_DUMP"),*dir=getenv("PW_FRAME_DIR");
    char name[4096];
    if(!is_target())return 0;
    frames++;last_width=width;last_height=height;
    if(dir) {
        snprintf(name,sizeof(name),"%s/%04lu.ppm",dir,frames);
        write_ppm(name,bgra,width,height,stride);
    }
    if(!path)return 0;
    snprintf(name,sizeof(name),"%s.tmp",path);
    write_ppm(name,bgra,width,height,stride);
    rename(name,path);
    return 0;
}

int pw_wine_next_input(struct input *event)
{
    const char *after=getenv("PW_INPUT_AFTER"),*click=getenv("PW_INPUT_CLICK");
    const char *moves=getenv("PW_INPUT_MOVES");
    int x,y;
    if(!is_target() || frames<(unsigned long)(after?atol(after):1))return 0;
    if(click && clicked<3 && sscanf(click,"%d,%d",&x,&y)==2) {
        *event=clicked==0?(struct input){2,0,x,y,0}:(struct input){3,0,0,0,clicked==1};
        clicked++;
        return 1;
    }
    if(!text)text=getenv("PW_INPUT_TEXT");
    if(text && typed<2*strlen(text)) {
        char c=text[typed/2];
        uint32_t vk=c==' '?0x20:(c>='a' && c<='z')?(uint32_t)(c-'a'+'A'):(uint32_t)c;
        *event=(struct input){1,vk,0,0,typed%2==0};
        typed++;
        return 1;
    }
    if(!moves || now_ms()-moved_ms<300)return 0;
    const char *point=moves;
    for(int i=0;i<moved && point;i++)if((point=strchr(point,';')))point++;
    if(!point || sscanf(point,"%d,%d",&x,&y)!=2)return 0;
    fprintf(stderr,"ps5 driver sink: move %d to %d,%d after frame %lu\n",moved,x,y,frames);
    moved++;moved_ms=now_ms();
    *event=(struct input){2,0,x,y,0};
    return 1;
}

__attribute__((destructor)) static void report(void)
{
    fprintf(stderr,"ps5 driver sink: frames=%lu size=%ux%u typed=%zu moved=%d\n",
            frames,last_width,last_height,typed/2,moved);
}
