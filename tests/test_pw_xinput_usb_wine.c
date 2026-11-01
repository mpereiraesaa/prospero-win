/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../native/pw_xinput_usb_wine.h"
#include <assert.h>
#include <errno.h>
#include <string.h>

static int pending,button=0x11,logs,disconnected,jitter,empty,axis=-32768;
static int enumerate(void *ctx,PwXinputUsbNode *nodes,size_t cap)
{
    (void)ctx;assert(cap>=2);
    nodes[0]=(PwXinputUsbNode){"/dev/usb/2.2.0",2,2,0};
    nodes[1]=(PwXinputUsbNode){"/dev/usb/2.2.1",2,2,1};
    return 2;
}
static int open_read(void *ctx,const char *path){(void)ctx;return path[strlen(path)-1]=='0'?100:101;}
static int descriptor(void *ctx,int fd,unsigned kind,uint8_t *out,size_t len)
{
    const uint8_t dev[18]={18,1,0,2,0xff,0xff,0xff,8,0x30,0x14,0x48,0x47,0,1};
    const uint8_t iface[9]={9,4,0,0,2,0xff,0x5d,1,0};
    const uint8_t ep[7]={7,5,0x81,3,32,0,4};
    (void)ctx;(void)fd;
    if(kind==1){assert(len==18);memcpy(out,dev,len);}
    else if(kind==4){assert(len==9);memcpy(out,iface,len);}
    else {assert(kind==5 && len==7);memcpy(out,ep,len);}
    return 0;
}
static int short_reads(void *ctx,int fd){(void)ctx;(void)fd;return 0;}
static ssize_t read_report(void *ctx,int fd,uint8_t *out,size_t len)
{
    (void)ctx;assert(fd==101 && len==64);
    if(disconnected)return -ENXIO;
    if(empty)return 0;
    if(!pending)return -EAGAIN;
    pending--;memset(out,0,20);out[1]=20;out[2]=(uint8_t)button;out[3]=(uint8_t)((0x9000|button)>>8);out[4]=(uint8_t)(120+jitter);out[12]=(uint8_t)(200+jitter);out[10]=(uint8_t)axis;out[11]=(uint8_t)((uint16_t)axis>>8);
    return 20;
}
static void close_fd(void *ctx,int fd){(void)ctx;assert(fd==100 || fd==101);}
static void log_line(const char *line){assert(strstr(line,"usb360"));logs++;}
int main(void)
{
    PwXinputUsbOps ops={NULL,enumerate,open_read,descriptor,short_reads,read_report,close_fd,NULL};
    PwXinputUsbWine bridge;
    PwWinePad pad={0},got;
    PwWinePadCaps caps;
    pad.buttons=0x2000;pw_wine_set_pad(&pad);
    assert(!pw_xinput_usb_wine_init(&bridge,&ops,pw_wine_set_pad_device_at,log_line,0));
    pending=1;pw_xinput_usb_wine_poll(&bridge,1,300);
    assert(pw_wine_pad_at(1,&got) && got.buttons==0x9011 && got.thumb_rx==-32768 && got.packet==1);
    assert(pw_wine_pad_caps_at(1,&caps) && caps.subtype==7 && caps.vendor_id==0x1430 && caps.product_id==0x4748);
    assert(!caps.flags && pw_wine_pad(&got) && got.buttons==0x2000);
    assert(!pw_wine_pad_at(2,&got) && !logs);
    pending=1;pw_xinput_usb_wine_poll(&bridge,2,300);
    assert(pw_wine_pad_at(1,&got) && got.packet==1);
    button=0x22;pending=1;pw_xinput_usb_wine_poll(&bridge,3,300);
    assert(pw_wine_pad_at(1,&got) && got.packet==2 && got.buttons==0x9022);
    empty=1;pw_xinput_usb_wine_poll(&bridge,4,300);
    assert(pw_wine_pad_at(1,&got) && got.packet==2 && got.buttons==0x9022 && !logs);
    empty=0;button=0x44;pending=1;pw_xinput_usb_wine_poll(&bridge,5,300);
    assert(pw_wine_pad_at(1,&got) && got.packet==3 && got.buttons==0x9044);
    disconnected=1;pw_xinput_usb_wine_poll(&bridge,6,300);
    assert(!pw_wine_pad_at(1,&got) && logs==1);
    disconnected=0;pending=1;pw_xinput_usb_wine_poll(&bridge,300,300);
    assert(pw_wine_pad_at(1,&got));
    pw_xinput_usb_wine_reserve(&bridge,3u); /* DualSense takes user 1 */
    assert(!pw_wine_pad_at(1,&got) && pw_wine_pad_at(2,&got));
    PwWinePad other={0};other.buttons=0x4000;
    pw_wine_set_pad_at(1,&other);
    pw_xinput_usb_wine_reserve(&bridge,15u); /* no free users: retain cached input */
    assert(!pw_wine_pad_at(2,&got) && pw_wine_pad_at(1,&got) && got.buttons==0x4000);
    pw_xinput_usb_wine_reserve(&bridge,3u);
    assert(pw_wine_pad_at(2,&got) && got.buttons==0x9044);
    pw_xinput_usb_wine_block(&bridge,1);
    assert(pw_wine_pad_at(2,&got) && !got.buttons);
    pw_xinput_usb_wine_block(&bridge,0);
    assert(pw_wine_pad_at(2,&got) && got.buttons==0x9044);
    pw_xinput_usb_wine_close(&bridge);
    assert(pw_wine_pad_at(1,&got) && got.buttons==0x4000); /* USB close never clears another owner */
    pw_wine_set_pad_at(1,NULL);
    disconnected=0;logs=0;
    assert(!pw_xinput_usb_wine_init(&bridge,&ops,NULL,log_line,1));
    pending=1;pw_xinput_usb_wine_poll(&bridge,1,300);
    assert(logs==1 && !pw_wine_pad_at(1,&got)); /* capture does not inject */
    pending=1;pw_xinput_usb_wine_poll(&bridge,2,300);assert(logs==1);
    button=0x33;pending=1;pw_xinput_usb_wine_poll(&bridge,3,300);assert(logs==2);
    assert(pw_wine_pad(&got) && got.buttons==0x2000);
    /* Thousands of idle transfers must not spend the button budget. */
    for(unsigned i=0;i<2000;i++) {
        jitter=(int)(i%3);pending=1;pw_xinput_usb_wine_poll(&bridge,4+i,0);
    }
    assert(logs==2 && bridge.button_records[0]==1 && bridge.analog_records[0]==1);
    for(unsigned bit=0;bit<16;bit++) {
        button=(int)(1u<<bit);pending=1;pw_xinput_usb_wine_poll(&bridge,2100+bit,0);
    }
    assert(logs==18 && bridge.button_records[0]==17 && !pw_wine_pad_at(1,&got));
    /* Slow movement is compared to last LOGGED analog, not each noisy packet. */
    int before=logs;
    for(axis=-32768;axis<=32700;axis+=512) {
        pending=1;pw_xinput_usb_wine_poll(&bridge,2200,0);
    }
    assert(logs>before && bridge.analog_records[0]>1);
    /* An exhausted analog channel cannot consume button evidence. */
    bridge.analog_records[0]=256;axis=-32768;pending=1;pw_xinput_usb_wine_poll(&bridge,2300,0);
    assert(bridge.analog_records[0]==257);
    before=logs;button=0x40;pending=1;pw_xinput_usb_wine_poll(&bridge,2301,0);
    assert(logs==before+1 && bridge.button_records[0]==18);
    assert(pw_wine_pad(&got) && got.buttons==0x2000);
    pw_xinput_usb_wine_close(&bridge);
    assert(!pw_xinput_usb_wine_init(&bridge,&ops,NULL,log_line,0) && !bridge.usb.limit);
    pw_xinput_usb_wine_poll(&bridge,1,300);pw_xinput_usb_wine_close(&bridge);
    return 0;
}
