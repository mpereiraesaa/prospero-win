/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_xinput_net.h"
#include <assert.h>
#include <string.h>

static void put16(uint8_t *p,unsigned v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}

/* The message PadBridge PS5 sends for a Wii Remote guitar. */
static void message(uint8_t m[PW_XINPUT_NET_SIZE],unsigned index,unsigned connected,unsigned buttons,int rx,uint32_t seq)
{
    memset(m,0,PW_XINPUT_NET_SIZE);memcpy(m,"PBXI",4);
    m[4]=1;m[5]=(uint8_t)index;m[6]=(uint8_t)connected;m[7]=7;
    put16(m+8,0x1430);put16(m+10,0x4748);put16(m+12,0x3122);put16(m+14,buttons);
    put16(m+18,(uint16_t)-32768);put16(m+20,32767);put16(m+22,(uint16_t)rx);
    put16(m+26,seq&0xffff);put16(m+28,seq>>16);
}

int main(void)
{
    uint8_t m[PW_XINPUT_NET_SIZE],bad[PW_XINPUT_NET_SIZE];
    PwXinputNetMessage got;

    message(m,2,1,0x1000|0x0002|0x0400,-32768,0x10002);
    assert(!pw_xinput_net_parse(m,sizeof(m),&got));
    assert(got.index==2 && got.connected==1 && got.subtype==7);
    assert(got.vendor_id==0x1430 && got.product_id==0x4748 && got.revision==0x3122);
    assert(got.buttons==(0x1000|0x0002));              /* 0x0400: not an XINPUT_GAMEPAD bit */
    assert(got.thumb_lx==-32768 && got.thumb_ly==32767 && got.thumb_rx==-32768 && got.thumb_ry==0);
    assert(got.sequence==0x10002);
    assert(pw_xinput_net_parse(m,sizeof(m)-1,&got)==-1);
    assert(pw_xinput_net_parse(NULL,sizeof(m),&got)==-1);
    for(unsigned at=0;at<8;at++) {                     /* magic, version, index, flags, subtype */
        static const uint8_t wrong[8]={'Q','Q','Q','Q',2,4,2,2};
        memcpy(bad,m,sizeof(m));bad[at]=wrong[at];
        assert(pw_xinput_net_parse(bad,sizeof(bad),&got)==-1);
    }
    memcpy(bad,m,sizeof(m));bad[31]=1;assert(pw_xinput_net_parse(bad,sizeof(bad),&got)==-1);
    memcpy(bad,m,sizeof(m));bad[7]=1;assert(!pw_xinput_net_parse(bad,sizeof(bad),&got) && got.subtype==1);

    /* Slots: highest free one, kept while it keeps coming; slot 0 never. */
    PwXinputNet net;
    uint8_t busy[PW_XINPUT_NET_SLOTS]={1,0,0,0};
    uint32_t slot=99;
    unsigned index=9;
    pw_xinput_net_init(&net);
    message(m,0,1,0,0,1);assert(!pw_xinput_net_parse(m,sizeof(m),&got));
    assert(pw_xinput_net_accept(&net,&got,1000,busy,&slot)==PW_XINPUT_NET_SET && slot==3);
    assert(pw_xinput_net_holds(&net,3) && !pw_xinput_net_holds(&net,2));
    busy[3]=1;                                          /* our own slot reads as connected */
    assert(pw_xinput_net_accept(&net,&got,1400,busy,&slot)==PW_XINPUT_NET_SET && slot==3);
    busy[2]=1;                                          /* a USB guitar has 1, and 2 is taken */
    message(m,1,1,0,0,1);assert(!pw_xinput_net_parse(m,sizeof(m),&got));
    busy[1]=1;
    assert(pw_xinput_net_accept(&net,&got,1400,busy,&slot)==PW_XINPUT_NET_IGNORE);   /* none free */
    busy[1]=0;
    assert(pw_xinput_net_accept(&net,&got,1450,busy,&slot)==PW_XINPUT_NET_SET && slot==1);
    /* Silence: source 0 last heard at 1400, so gone at 1900; source 1 at 1950. */
    assert(!pw_xinput_net_expire(&net,1899,&slot,&index));
    assert(pw_xinput_net_expire(&net,1900,&slot,&index)==1 && slot==3 && index==0);
    assert(!pw_xinput_net_expire(&net,1900,&slot,&index));
    assert(!pw_xinput_net_holds(&net,3) && pw_xinput_net_holds(&net,1));
    /* "Gone" message: cleared at once, and a second one is ignored. */
    message(m,1,0,0,0,2);assert(!pw_xinput_net_parse(m,sizeof(m),&got) && !got.connected);
    assert(pw_xinput_net_accept(&net,&got,1500,busy,&slot)==PW_XINPUT_NET_CLEAR && slot==1);
    assert(pw_xinput_net_accept(&net,&got,1501,busy,&slot)==PW_XINPUT_NET_IGNORE);
    assert(!pw_xinput_net_expire(&net,99999,&slot,&index));
    return 0;
}
