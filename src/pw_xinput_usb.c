/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_xinput_usb.h"
#include <string.h>

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0]|((uint16_t)p[1]<<8)); }
static int16_t s16(const uint8_t *p)
{
    uint16_t value=u16(p);
    return value<0x8000u?(int16_t)value:(int16_t)((int32_t)value-65536);
}
int pw_xinput_usb_descriptors(const uint8_t *dev,size_t dl,const uint8_t *iface,size_t il,
                              const uint8_t *ep,size_t el,PwXinputUsbDevice *out)
{
    PwXinputUsbDevice device;
    if(!out)return 0;
    memset(out,0,sizeof(*out));
    if(!dev || !iface || !ep || dl<18 || il<9 || el<7)return 0;
    if(dev[0]!=18 || dev[1]!=1 || iface[0]!=9 || iface[1]!=4 || iface[3]!=0 ||
       iface[5]!=0xff || iface[6]!=0x5d || iface[7]!=1 ||
       ep[0]!=7 || ep[1]!=5 || (ep[2]&0x80)==0 || (ep[2]&0x0f)==0 ||
       (ep[2]&0x70)!=0 || (ep[3]&3)!=3)return 0;
    memset(&device,0,sizeof(device));
    device.max_packet=u16(ep+4);
    if(device.max_packet<PW_XINPUT_USB_REPORT || device.max_packet>PW_XINPUT_USB_PACKET_MAX)return 0;
    device.vendor_id=u16(dev+8);device.product_id=u16(dev+10);device.revision=u16(dev+12);
    device.interface_number=iface[2];device.endpoint_address=ep[2];
    /* Unlisted wired-360 devices retain gamepad subtype. Identity comes
     * from descriptors; only a known guitar gets a guitar-specific layout. */
    device.subtype=device.vendor_id==0x1430 && device.product_id==0x4748?7:1;
    *out=device;
    return 1;
}
int pw_xinput_usb_report(const uint8_t *data,size_t length,PwXinputUsbState *out)
{
    if(!out)return 0;
    memset(out,0,sizeof(*out));
    if(!data || length!=PW_XINPUT_USB_REPORT || data[0]!=0 || data[1]!=PW_XINPUT_USB_REPORT)return 0;
    out->buttons=u16(data+2);out->left_trigger=data[4];out->right_trigger=data[5];
    out->thumb_lx=s16(data+6);out->thumb_ly=s16(data+8);
    out->thumb_rx=s16(data+10);out->thumb_ry=s16(data+12);
    return 1;
}
