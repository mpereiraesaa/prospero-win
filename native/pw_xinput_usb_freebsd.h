/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_XINPUT_USB_FREEBSD_H
#define PW_XINPUT_USB_FREEBSD_H
#include "../src/pw_xinput_usb.h"
#include <sys/types.h>

enum { PW_XINPUT_USB_DEVICES=4, PW_XINPUT_USB_NODES=128,
       PW_XINPUT_USB_DEVICE_DESC=1, PW_XINPUT_USB_INTERFACE_DESC=4, PW_XINPUT_USB_ENDPOINT_DESC=5 };
typedef struct PwXinputUsbNode {
    char path[64];
    unsigned bus,address,node;
} PwXinputUsbNode;
/* All failures are negative errno values. Enumeration failure leaves
 * existing connections intact. This seam also exercises the real backend
 * lifecycle on a host without touching USB hardware. */
typedef struct PwXinputUsbOps {
    void *context;
    int (*enumerate)(void *,PwXinputUsbNode *,size_t);
    int (*open_read)(void *,const char *);
    int (*descriptor)(void *,int,unsigned,uint8_t *,size_t);
    int (*short_reads)(void *,int);
    ssize_t (*read)(void *,int,uint8_t *,size_t);
    void (*close)(void *,int);
    void (*log)(void *,const char *);
} PwXinputUsbOps;
typedef void (*PwXinputUsbReport)(void *,unsigned,const PwXinputUsbDevice *,
                                 const PwXinputUsbState *,const uint8_t *);
typedef struct PwXinputUsbFreebsd {
    PwXinputUsbOps ops;
    PwXinputUsbReport report;
    void *context;
    unsigned limit;
    struct {
        int fd,has_report;
        uint32_t empty_reads;
        PwXinputUsbNode node;
        PwXinputUsbDevice device;
    } slots[PW_XINPUT_USB_DEVICES];
} PwXinputUsbFreebsd;
/* No worker, configuration request, detach/claim, reset or output traffic.
 * Call from the title's input owner after credentials are granted. */
int pw_xinput_usb_freebsd_init(PwXinputUsbFreebsd *,const PwXinputUsbOps *,unsigned limit,
                               PwXinputUsbReport,void *context);
int pw_xinput_usb_freebsd_scan(PwXinputUsbFreebsd *);
void pw_xinput_usb_freebsd_poll(PwXinputUsbFreebsd *);
void pw_xinput_usb_freebsd_close(PwXinputUsbFreebsd *);
const PwXinputUsbOps *pw_xinput_usb_freebsd_system_ops(void);
#endif
