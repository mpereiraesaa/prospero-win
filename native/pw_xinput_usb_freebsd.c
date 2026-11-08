/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _DEFAULT_SOURCE
#include "pw_xinput_usb_freebsd.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* FreeBSD 9 dev/usb/usb_ioctl.h: cached copy-out descriptors, no bus requests.
 * Explicit encoding also permits host tests on a Linux build machine. */
#define PW_USB_IOR(n,len) (0x40000000UL|((unsigned long)(len)<<16)|('U'<<8)|(n))
#define PW_USB_IOW(n,len) (0x80000000UL|((unsigned long)(len)<<16)|('U'<<8)|(n))

static int system_enumerate(void *context,PwXinputUsbNode *nodes,size_t capacity)
{
    DIR *dir;
    struct dirent *entry;
    size_t count=0;
    int error=0;
    (void)context;
    dir=opendir("/dev/usb");
    if(!dir)return -errno;
    for(;;) {
        unsigned bus,address,node;
        char extra;
        errno=0;entry=readdir(dir);
        if(!entry){error=errno;break;}
        if(sscanf(entry->d_name,"%u.%u.%u%c",&bus,&address,&node,&extra)!=3 ||
           bus>255 || address>255 || node>31)continue;
        if(count>=capacity){error=EOVERFLOW;break;}
        nodes[count].bus=bus;nodes[count].address=address;nodes[count].node=node;
        snprintf(nodes[count].path,sizeof(nodes[count].path),"/dev/usb/%u.%u.%u",bus,address,node);
        count++;
    }
    closedir(dir);
    return error?-error:(int)count;
}
static int system_open(void *context,const char *path)
{
    int fd;
    (void)context;
    fd=open(path,O_RDONLY|O_NONBLOCK);
    return fd<0?-errno:fd;
}
static int system_descriptor(void *context,int fd,unsigned kind,uint8_t *data,size_t length)
{
    unsigned long request;
    (void)context;
    if(kind==PW_XINPUT_USB_DEVICE_DESC && length==18)request=PW_USB_IOR(105,18);
    else if(kind==PW_XINPUT_USB_INTERFACE_DESC && length==9)request=PW_USB_IOR(107,9);
    else if(kind==PW_XINPUT_USB_ENDPOINT_DESC && length==7)request=PW_USB_IOR(108,7);
    else return -EINVAL;
    return ioctl(fd,request,data)<0?-errno:0;
}
static int system_short_reads(void *context,int fd)
{
    int value=1;
    (void)context;
    return ioctl(fd,PW_USB_IOW(113,sizeof(value)),&value)<0?-errno:0;
}
static ssize_t system_read(void *context,int fd,uint8_t *data,size_t capacity)
{
    ssize_t n;
    (void)context;
    n=read(fd,data,capacity);
    return n<0?-errno:n;
}
static void system_close(void *context,int fd) { (void)context;close(fd); }
const PwXinputUsbOps *pw_xinput_usb_freebsd_system_ops(void)
{
    static const PwXinputUsbOps ops={NULL,system_enumerate,system_open,system_descriptor,
                                    system_short_reads,system_read,system_close,NULL};
    return &ops;
}
static int descriptor(PwXinputUsbFreebsd *usb,int fd,unsigned kind,uint8_t *data,size_t length)
{
    memset(data,0,length);
    return usb->ops.descriptor(usb->ops.context,fd,kind,data,length);
}
static void disconnect(PwXinputUsbFreebsd *usb,unsigned slot)
{
    if(usb->slots[slot].fd<0)return;
    usb->ops.close(usb->ops.context,usb->slots[slot].fd);
    usb->slots[slot].fd=-1;
    if(usb->slots[slot].has_report && usb->report)
        usb->report(usb->context,slot,&usb->slots[slot].device,NULL,NULL);
    usb->slots[slot].has_report=0;
}
int pw_xinput_usb_freebsd_init(PwXinputUsbFreebsd *usb,const PwXinputUsbOps *ops,unsigned limit,
                               PwXinputUsbReport report,void *context)
{
    if(!usb || !ops || !ops->enumerate || !ops->open_read || !ops->descriptor ||
       !ops->short_reads || !ops->read || !ops->close || !limit || limit>PW_XINPUT_USB_DEVICES)return -EINVAL;
    memset(usb,0,sizeof(*usb));usb->ops=*ops;usb->limit=limit;usb->report=report;usb->context=context;
    for(unsigned i=0;i<PW_XINPUT_USB_DEVICES;i++)usb->slots[i].fd=-1;
    return 0;
}
int pw_xinput_usb_freebsd_scan(PwXinputUsbFreebsd *usb)
{
    PwXinputUsbNode nodes[PW_XINPUT_USB_NODES];
    int count;
    if(!usb || !usb->limit)return -EINVAL;
    memset(nodes,0,sizeof(nodes));
    count=usb->ops.enumerate(usb->ops.context,nodes,PW_XINPUT_USB_NODES);
    if(count<0)return count;
    if(count>PW_XINPUT_USB_NODES)return -EOVERFLOW;
    for(unsigned s=0;s<usb->limit;s++) {
        int present=0;
        if(usb->slots[s].fd<0)continue;
        for(int i=0;i<count;i++)if(!strcmp(nodes[i].path,usb->slots[s].node.path))present=1;
        if(!present)disconnect(usb,s);
    }
    for(int i=0;i<count;i++) {
        uint8_t dev[18],iface[9];
        int control,known=0;
        if(nodes[i].node!=0)continue;
        for(unsigned s=0;s<usb->limit;s++)if(usb->slots[s].fd>=0 &&
            usb->slots[s].node.bus==nodes[i].bus && usb->slots[s].node.address==nodes[i].address)known=1;
        if(known)continue;
        unsigned slot=0;
        while(slot<usb->limit && usb->slots[slot].fd>=0)slot++;
        if(slot==usb->limit)break;
        control=usb->ops.open_read(usb->ops.context,nodes[i].path);
        if(control<0)continue;
        int ok=!descriptor(usb,control,PW_XINPUT_USB_DEVICE_DESC,dev,sizeof(dev)) &&
               !descriptor(usb,control,PW_XINPUT_USB_INTERFACE_DESC,iface,sizeof(iface)) &&
               iface[0]==9 && iface[1]==4 && !iface[3] && iface[5]==0xff && iface[6]==0x5d && iface[7]==1;
        usb->ops.close(usb->ops.context,control);
        if(!ok)continue;
        for(int j=0;j<count;j++) {
            uint8_t ed[7],id[9],dd[18];
            PwXinputUsbDevice device;
            int fd;
            if(!nodes[j].node || nodes[j].bus!=nodes[i].bus || nodes[j].address!=nodes[i].address)continue;
            fd=usb->ops.open_read(usb->ops.context,nodes[j].path);
            if(fd<0) {
                if(usb->ops.log) {
                    char line[128];snprintf(line,sizeof(line),"usb360 endpoint path=%.63s open_errno=%d",nodes[j].path,-fd);
                    usb->ops.log(usb->ops.context,line);
                }
                continue;
            }
            int drc=descriptor(usb,fd,PW_XINPUT_USB_DEVICE_DESC,dd,sizeof(dd));
            int irc=descriptor(usb,fd,PW_XINPUT_USB_INTERFACE_DESC,id,sizeof(id));
            int erc=descriptor(usb,fd,PW_XINPUT_USB_ENDPOINT_DESC,ed,sizeof(ed));
            ok=!drc && !irc && !erc && !memcmp(dev,dd,sizeof(dev)) &&
               pw_xinput_usb_descriptors(dd,sizeof(dd),id,sizeof(id),ed,sizeof(ed),&device);
            int short_rc=ok?usb->ops.short_reads(usb->ops.context,fd):0;
            if(!ok || short_rc) {
                if(usb->ops.log) {
                    char line[192];
                    snprintf(line,sizeof(line),"usb360 rejected path=%.63s desc_rc=%d/%d/%d if=%02x/%02x/%02x ep=%02x short_rc=%d",
                             nodes[j].path,drc,irc,erc,id[5],id[6],id[7],ed[2],short_rc);
                    usb->ops.log(usb->ops.context,line);
                }
                usb->ops.close(usb->ops.context,fd);continue;
            }
            usb->slots[slot].fd=fd;usb->slots[slot].node=nodes[j];usb->slots[slot].device=device;
            usb->slots[slot].empty_reads=0;
            if(usb->ops.log) {
                char line[192];
                snprintf(line,sizeof(line),"usb360 ready slot=%u path=%.63s vid=%04x pid=%04x if=%u ep=%02x subtype=%u",
                         slot,nodes[j].path,device.vendor_id,device.product_id,device.interface_number,
                         device.endpoint_address,device.subtype);
                usb->ops.log(usb->ops.context,line);
            }
            break;
        }
    }
    return 0;
}
void pw_xinput_usb_freebsd_poll(PwXinputUsbFreebsd *usb)
{
    if(!usb || !usb->limit)return;
    for(unsigned slot=0;slot<usb->limit;slot++) {
        if(usb->slots[slot].fd<0)continue;
        for(unsigned budget=0;budget<8;budget++) {
            uint8_t data[PW_XINPUT_USB_PACKET_MAX];
            PwXinputUsbState state;
            ssize_t n=usb->ops.read(usb->ops.context,usb->slots[slot].fd,data,sizeof(data));
            if(n==-EAGAIN || n==-EWOULDBLOCK || n==-EINTR)break;
            /* A USB transfer may contain zero bytes. It is not a devfs
             * disconnect: preserve the newest state and wait for a later
             * poll. Removal is a real read error or a missing scanned node. */
            if(n==0) {
                uint32_t empty=++usb->slots[slot].empty_reads;
                if(usb->ops.log && empty && !(empty&(empty-1))) {
                    char line[96];snprintf(line,sizeof(line),"usb360 empty slot=%u total=%u has_report=%d",
                                          slot,empty,usb->slots[slot].has_report);
                    usb->ops.log(usb->ops.context,line);
                }
                break;
            }
            if(n<0 || (size_t)n>sizeof(data)) {
                if(usb->ops.log) {
                    char line[96];snprintf(line,sizeof(line),"usb360 read stopped slot=%u result=%ld",slot,(long)n);
                    usb->ops.log(usb->ops.context,line);
                }
                disconnect(usb,slot);break;
            }
            if(!pw_xinput_usb_report(data,(size_t)n,&state))continue;
            usb->slots[slot].has_report=1;
            if(usb->report)usb->report(usb->context,slot,&usb->slots[slot].device,&state,data);
        }
    }
}
void pw_xinput_usb_freebsd_close(PwXinputUsbFreebsd *usb)
{
    if(!usb)return;
    for(unsigned slot=0;slot<usb->limit;slot++)disconnect(usb,slot);
    usb->limit=0;
}
