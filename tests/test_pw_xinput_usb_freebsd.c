/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../native/pw_xinput_usb_freebsd.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

typedef struct Fixture {
    int devices,enumerate_error,short_error,mode[4],pending[4],reads[4],closed,endpoint_opens;
    int reports,disconnects;
    unsigned seen_slot;
    PwXinputUsbState state;
    uint8_t raw[20];
} Fixture;
static int enumerate(void *context,PwXinputUsbNode *nodes,size_t cap)
{
    Fixture *f=context;
    if(f->enumerate_error)return -EACCES;
    assert(cap>=8);
    for(int device=0;device<f->devices;device++)for(unsigned node=0;node<2;node++) {
        PwXinputUsbNode *n=&nodes[2*device+node];
        n->bus=2;n->address=(unsigned)device+2;n->node=node;
        snprintf(n->path,sizeof(n->path),"/dev/usb/2.%u.%u",n->address,node);
    }
    return 2*f->devices;
}
static int open_read(void *context,const char *path)
{
    Fixture *f=context;
    unsigned address,node;
    assert(sscanf(path,"/dev/usb/2.%u.%u",&address,&node)==2);
    if(node)f->endpoint_opens++;
    return 100+(int)address*10+(int)node;
}
static int descriptor(void *context,int fd,unsigned kind,uint8_t *data,size_t length)
{
    (void)context;
    unsigned device=(unsigned)(fd-100)/10-2;
    const uint8_t dev[18]={18,1,0,2,0xff,0xff,0xff,8,0x30,0x14,0x48,0x47,0,1};
    uint8_t iface[9]={9,4,0,0,2,0xff,0x5d,1,0};
    const uint8_t ep[7]={7,5,0x81,3,32,0,4};
    if(device==3)iface[5]=3; /* unrelated HID device: never open its endpoints */
    if(kind==PW_XINPUT_USB_DEVICE_DESC){assert(length==18);memcpy(data,dev,18);}
    else if(kind==PW_XINPUT_USB_INTERFACE_DESC){assert(length==9);memcpy(data,iface,9);}
    else {assert(kind==PW_XINPUT_USB_ENDPOINT_DESC && length==7);memcpy(data,ep,7);}
    return 0;
}
static int short_reads(void *context,int fd)
{
    Fixture *f=context;(void)fd;return f->short_error?-EIO:0;
}
static ssize_t read_report(void *context,int fd,uint8_t *data,size_t capacity)
{
    Fixture *f=context;
    unsigned device=(unsigned)(fd-100)/10-2;
    assert(capacity==64);f->reads[device]++;
    if(f->mode[device]==3)return -EINTR;
    if(f->mode[device]==4)return -ENXIO;
    if(f->mode[device]==5)return 0;
    if(f->mode[device]==6)return 65;
    if(!f->pending[device])return -EAGAIN;
    f->pending[device]--;
    memset(data,0,20);data[1]=20;data[2]=0x11;data[3]=0x90;data[10]=0xff;data[11]=0x7f;
    if(f->mode[device]==2)data[0]=1;
    return 20;
}
static void close_fd(void *context,int fd){ Fixture *f=context;assert(fd>=100);f->closed++; }
static void report(void *context,unsigned slot,const PwXinputUsbDevice *device,
                    const PwXinputUsbState *state,const uint8_t *raw)
{
    Fixture *f=context;assert(device->vendor_id==0x1430 && device->product_id==0x4748 && device->subtype==7);
    f->seen_slot=slot;
    if(!state){assert(!raw);f->disconnects++;return;}
    f->reports++;f->state=*state;memcpy(f->raw,raw,20);
}
int main(void)
{
    Fixture f={0};
    PwXinputUsbFreebsd usb;
    PwXinputUsbOps ops={&f,enumerate,open_read,descriptor,short_reads,read_report,close_fd,NULL};
    assert(pw_xinput_usb_freebsd_init(NULL,&ops,3,report,&f)==-EINVAL);
    assert(pw_xinput_usb_freebsd_init(&usb,NULL,3,report,&f)==-EINVAL);
    assert(pw_xinput_usb_freebsd_init(&usb,&ops,0,report,&f)==-EINVAL);
    assert(!pw_xinput_usb_freebsd_init(&usb,&ops,4,report,&f));
    assert(!pw_xinput_usb_freebsd_scan(&usb));
    f.devices=4;
    assert(!pw_xinput_usb_freebsd_scan(&usb) && f.endpoint_opens==3);
    assert(usb.slots[0].fd==121 && usb.slots[1].fd==131 && usb.slots[2].fd==141 && usb.slots[3].fd==-1);
    assert(!f.reports && !f.disconnects); /* opening alone never connects an XInput slot */
    f.pending[1]=1;pw_xinput_usb_freebsd_poll(&usb);
    assert(f.reports==1 && f.seen_slot==1 && f.state.buttons==0x9011 && f.state.thumb_rx==32767);
    assert(f.raw[1]==20);
    int opened=f.endpoint_opens;
    assert(!pw_xinput_usb_freebsd_scan(&usb) && f.endpoint_opens==opened);
    f.pending[0]=20;int reads=f.reads[0];pw_xinput_usb_freebsd_poll(&usb);
    assert(f.reads[0]-reads==8 && f.pending[0]==12); /* bounded per poll */
    f.mode[0]=2;pw_xinput_usb_freebsd_poll(&usb);assert(f.pending[0]==4);
    f.mode[0]=3;pw_xinput_usb_freebsd_poll(&usb);assert(f.pending[0]==4 && usb.slots[0].fd>=0);
    f.enumerate_error=1;
    assert(pw_xinput_usb_freebsd_scan(&usb)==-EACCES && usb.slots[0].fd>=0);
    f.enumerate_error=0;
    f.mode[1]=4;pw_xinput_usb_freebsd_poll(&usb);
    assert(usb.slots[1].fd==-1 && f.disconnects==1);
    f.mode[1]=0;f.short_error=1;assert(!pw_xinput_usb_freebsd_scan(&usb) && usb.slots[1].fd==-1);
    f.short_error=0;assert(!pw_xinput_usb_freebsd_scan(&usb) && usb.slots[1].fd==131);
    f.pending[1]=1;pw_xinput_usb_freebsd_poll(&usb);assert(f.seen_slot==1);
    f.devices=1;assert(!pw_xinput_usb_freebsd_scan(&usb) && usb.slots[1].fd==-1 && f.disconnects==2);
    int closed=f.closed,reports=f.reports;
    f.mode[0]=5;pw_xinput_usb_freebsd_poll(&usb);
    assert(usb.slots[0].fd==121 && f.disconnects==2 && f.closed==closed && f.reports==reports);
    f.mode[0]=0;f.pending[0]=1;pw_xinput_usb_freebsd_poll(&usb);
    assert(usb.slots[0].fd==121 && f.reports==reports+1); /* stream resumes without reopening */
    f.mode[0]=6;pw_xinput_usb_freebsd_poll(&usb);
    assert(usb.slots[0].fd==-1 && f.disconnects==3); /* impossible oversized result is an error */
    f.mode[0]=0;assert(!pw_xinput_usb_freebsd_scan(&usb));
    f.devices=0;assert(!pw_xinput_usb_freebsd_scan(&usb) && usb.slots[0].fd==-1);
    assert(f.disconnects==3); /* no report was delivered after this reopen */
    pw_xinput_usb_freebsd_close(&usb);pw_xinput_usb_freebsd_close(&usb);
    assert(pw_xinput_usb_freebsd_scan(&usb)==-EINVAL);
    assert(pw_xinput_usb_freebsd_system_ops()->open_read!=NULL);
    return 0;
}
