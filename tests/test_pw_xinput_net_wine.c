/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../native/pw_xinput_net_wine.h"
#include <assert.h>
#include <errno.h>
#include <string.h>

/* A loopback socket that hands out queued datagrams. */
static uint8_t queue[16][40];
static long lengths[16];
static int queued,taken,opened,closed,open_error,receive_error,logs;
static char last_log[160];

static int fake_open(void *ctx,uint16_t port){(void)ctx;assert(port==8097);opened++;return open_error?-open_error:7;}
static long fake_receive(void *ctx,int fd,uint8_t *data,size_t cap)
{
    (void)ctx;assert(fd==7 && cap>=PW_XINPUT_NET_SIZE);
    if(receive_error)return -receive_error;
    if(taken==queued)return 0;
    long n=lengths[taken];memcpy(data,queue[taken],(size_t)n);taken++;
    return n;
}
static void fake_close(void *ctx,int fd){(void)ctx;assert(fd==7);closed++;}
static void log_line(const char *line){assert(strstr(line,"netxinput"));strncpy(last_log,line,sizeof(last_log)-1);logs++;}

static void put16(uint8_t *p,unsigned v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void push(unsigned index,unsigned connected,unsigned buttons,int rx)
{
    uint8_t *m=queue[queued];
    memset(m,0,PW_XINPUT_NET_SIZE);memcpy(m,"PBXI",4);
    m[4]=1;m[5]=(uint8_t)index;m[6]=(uint8_t)connected;m[7]=7;
    put16(m+8,0x1430);put16(m+10,0x4748);put16(m+12,0x3122);put16(m+14,buttons);put16(m+22,(uint16_t)rx);
    lengths[queued++]=PW_XINPUT_NET_SIZE;
}

int main(void)
{
    PwXinputNetOps ops={fake_open,fake_receive,fake_close,NULL};
    PwXinputNetWine bridge;
    PwWinePad pad={0},got;
    PwWinePadCaps caps;

    /* No device sink: off, no socket. */
    assert(!pw_xinput_net_wine_init(&bridge,&ops,NULL,log_line) && !opened && bridge.fd<0);
    pw_xinput_net_wine_poll(&bridge,0);
    /* A socket that cannot be had: logged, off. */
    open_error=EADDRINUSE;
    assert(pw_xinput_net_wine_init(&bridge,&ops,pw_wine_set_pad_device_at,log_line)==-EADDRINUSE);
    assert(strstr(last_log,"unavailable") && bridge.fd<0);
    open_error=0;

    /* The DualSense on user 0, a wired guitar on user 1: the Wii guitar gets 3. */
    pad.buttons=0x2000;pw_wine_set_pad(&pad);
    pad.buttons=0x0100;pw_wine_set_pad_device_at(1,&pad,NULL);
    assert(!pw_xinput_net_wine_init(&bridge,&ops,pw_wine_set_pad_device_at,log_line));
    assert(strstr(last_log,"listening 127.0.0.1:8097"));
    pw_xinput_net_wine_reserve(&bridge,2u);
    push(0,1,0x1000|0x0002,-32768);
    pw_xinput_net_wine_poll(&bridge,1000);
    assert(pw_wine_pad_at(3,&got) && got.buttons==(0x1000|0x0002) && got.thumb_rx==-32768);
    assert(pw_wine_pad_caps_at(3,&caps) && caps.subtype==7 && caps.vendor_id==0x1430 && caps.product_id==0x4748);
    assert(strstr(last_log,"source=0 xinput=3 subtype=7") && pw_xinput_net_wine_users(&bridge)==8u);
    assert(pw_wine_pad(&got) && got.buttons==0x2000);           /* the DualSense untouched */
    assert(pw_wine_pad_at(1,&got) && got.buttons==0x0100);      /* the wired guitar too */

    /* Whammy down, then junk and an oversized datagram: rejected, the state kept. */
    push(0,1,0x1000,32767);
    memset(queue[queued],0,40);lengths[queued++]=33;
    memcpy(queue[queued],"NOPE",4);lengths[queued++]=PW_XINPUT_NET_SIZE;
    pw_xinput_net_wine_poll(&bridge,1050);
    assert(pw_wine_pad_at(3,&got) && got.buttons==0x1000 && got.thumb_rx==32767);
    assert(bridge.received==2 && bridge.rejected==2);

    /* The menu open: connected but idle, and the state back once it closes. */
    pw_xinput_net_wine_block(&bridge,1);
    assert(pw_wine_pad_at(3,&got) && !got.buttons && !got.thumb_rx);
    push(0,1,0x1000,32767);
    pw_xinput_net_wine_poll(&bridge,1060);
    assert(pw_wine_pad_at(3,&got) && !got.buttons);
    pw_xinput_net_wine_block(&bridge,0);
    assert(pw_wine_pad_at(3,&got) && got.buttons==0x1000 && got.thumb_rx==32767);

    /* A second source takes user 2; "gone" frees it at once. */
    push(1,1,0x8000,0);
    pw_xinput_net_wine_poll(&bridge,1100);
    assert(pw_wine_pad_at(2,&got) && got.buttons==0x8000 && pw_xinput_net_wine_users(&bridge)==12u);
    push(1,0,0,0);
    pw_xinput_net_wine_poll(&bridge,1150);
    assert(!pw_wine_pad_at(2,&got) && strstr(last_log,"source=1 gone xinput=2 (message)"));

    /* A user signs in on 3: the source leaves it before the DualSense
     * writes it, and its next message takes the free user 2. */
    pw_xinput_net_wine_reserve(&bridge,2u|8u);
    assert(!pw_wine_pad_at(3,&got) && strstr(last_log,"source=0 gone xinput=3 (reserved)"));
    assert(!pw_xinput_net_wine_users(&bridge));
    pad.buttons=0x0200;pw_wine_set_pad_at(3,&pad);
    push(0,1,0x1000,0);
    pw_xinput_net_wine_poll(&bridge,1200);
    assert(pw_wine_pad_at(2,&got) && got.buttons==0x1000 && strstr(last_log,"source=0 xinput=2"));
    assert(pw_wine_pad_at(3,&got) && got.buttons==0x0200);      /* the new user untouched */
    /* No user free: the message is ignored and nothing is overwritten. */
    pw_xinput_net_wine_reserve(&bridge,15u);
    push(0,1,0x1000,0);
    pw_xinput_net_wine_poll(&bridge,1250);
    assert(strstr(last_log,"no free xinput slot") && !pw_xinput_net_wine_users(&bridge));
    assert(pw_wine_pad_at(1,&got) && got.buttons==0x0100 && pw_wine_pad_at(3,&got) && got.buttons==0x0200);
    pw_wine_set_pad_at(3,NULL);
    pw_xinput_net_wine_reserve(&bridge,2u);
    push(0,1,0x1000,0);
    pw_xinput_net_wine_poll(&bridge,1300);
    assert(pw_wine_pad_at(3,&got) && got.buttons==0x1000);

    /* Silence: the source is let go 500 ms after its last message. */
    pw_xinput_net_wine_poll(&bridge,1799);
    assert(pw_wine_pad_at(3,&got));
    pw_xinput_net_wine_poll(&bridge,1800);
    assert(!pw_wine_pad_at(3,&got) && strstr(last_log,"source=0 gone xinput=3 (timeout)"));

    /* Back, then closed: its user is cleared and the socket closed. */
    push(0,1,0x4000,0);
    pw_xinput_net_wine_poll(&bridge,2000);
    assert(pw_wine_pad_at(3,&got) && got.buttons==0x4000);
    pw_xinput_net_wine_close(&bridge);
    assert(!pw_wine_pad_at(3,&got) && closed==1 && bridge.fd<0);
    assert(pw_wine_pad_at(1,&got) && pw_wine_pad(&got));         /* others untouched */

    /* A socket error stops the bridge and frees its users. */
    taken=queued=0;
    assert(!pw_xinput_net_wine_init(&bridge,&ops,pw_wine_set_pad_device_at,log_line));
    push(0,1,0x1000,0);
    pw_xinput_net_wine_poll(&bridge,3000);
    assert(pw_wine_pad_at(3,&got));
    receive_error=ECONNRESET;
    pw_xinput_net_wine_poll(&bridge,3010);
    assert(!pw_wine_pad_at(3,&got) && bridge.fd<0 && closed==2 && strstr(last_log,"stopped"));
    return 0;
}
