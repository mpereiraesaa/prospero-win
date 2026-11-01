/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_XINPUT_USB_WINE_H
#define PW_XINPUT_USB_WINE_H
#include "pw_xinput_usb_freebsd.h"
#include "../wine/ps5/pw_wine_sink.h"
typedef int (*PwXinputUsbSetSlot)(uint32_t,const PwWinePad *,const PwWinePadCaps *);
typedef struct PwXinputUsbWine {
    PwXinputUsbFreebsd usb;
    PwXinputUsbSetSlot set_slot;
    void (*log)(const char *);
    unsigned capture,records[3],button_records[3],analog_records[3];
    uint32_t reserved;
    unsigned index[3],connected[3],blocked;
    PwWinePad pad[3];
    PwWinePadCaps caps[3];
    PwXinputUsbState last[3];
} PwXinputUsbWine;
/* User 0 stays the title pad's. USB devices use available users 1..3.
 * Reserve other signed-in DualSenses before feeding them; existing USB users
 * keep their index until it is reserved, then move to a free index or wait.
 * Without the device-capability sink, normal mode stays disabled. */
void pw_xinput_usb_wine_reserve(PwXinputUsbWine *,uint32_t users);
void pw_xinput_usb_wine_block(PwXinputUsbWine *,int blocked);
int pw_xinput_usb_wine_init(PwXinputUsbWine *,const PwXinputUsbOps *,PwXinputUsbSetSlot,
                            void (*log)(const char *),int capture);
void pw_xinput_usb_wine_poll(PwXinputUsbWine *,uint64_t tick,uint64_t retry_ticks);
void pw_xinput_usb_wine_close(PwXinputUsbWine *);
#endif
