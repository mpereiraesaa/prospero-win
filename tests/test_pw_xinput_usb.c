/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_xinput_usb.h"
#include <assert.h>
#include <string.h>
#include "fixtures/xplorer_1430_4748.h"

int main(void)
{
    uint8_t dev[18]={18,1,0,2,0xff,0xff,0xff,8,0x30,0x14,0x48,0x47,0,1};
    uint8_t iface[9]={9,4,0,0,2,0xff,0x5d,1,0};
    uint8_t ep[7]={7,5,0x81,3,32,0,4};
    uint8_t report[21]={0,20,0x21,0x91,0xff,0x80,0,0x80,0xff,0x7f,0xff,0xff,0x34,0x12};
    PwXinputUsbDevice d;
    PwXinputUsbState state;
    assert(pw_xinput_usb_descriptors(dev,18,iface,9,ep,7,&d));
    assert(d.vendor_id==0x1430 && d.product_id==0x4748 && d.revision==0x0100 && d.subtype==7);
    assert(d.endpoint_address==0x81 && d.max_packet==32 && !d.interface_number);
    dev[8]=0x5e;dev[9]=4;dev[10]=0x8e;dev[11]=2;
    assert(pw_xinput_usb_descriptors(dev,18,iface,9,ep,7,&d) && d.subtype==1);
    for(size_t n=0;n<18;n++)assert(!pw_xinput_usb_descriptors(dev,n,iface,9,ep,7,&d));
    for(size_t n=0;n<9;n++)assert(!pw_xinput_usb_descriptors(dev,18,iface,n,ep,7,&d));
    for(size_t n=0;n<7;n++)assert(!pw_xinput_usb_descriptors(dev,18,iface,9,ep,n,&d));
    const unsigned ib[]={0,1,3,5,6,7};
    for(unsigned i=0;i<sizeof(ib)/sizeof(ib[0]);i++) {
        iface[ib[i]]^=1;assert(!pw_xinput_usb_descriptors(dev,18,iface,9,ep,7,&d));iface[ib[i]]^=1;
    }
    const uint8_t invalid_ep[][7]={{7,5,1,3,32,0,4},{7,5,0x80,3,32,0,4},
        {7,5,0x81,2,32,0,4},{7,5,0x81,3,19,0,4},{7,5,0x81,3,65,0,4},
        {7,5,0x91,3,32,0,4},{7,5,0x81,3,32,8,4}};
    for(unsigned i=0;i<sizeof(invalid_ep)/sizeof(invalid_ep[0]);i++)
        assert(!pw_xinput_usb_descriptors(dev,18,iface,9,invalid_ep[i],7,&d));
    assert(!pw_xinput_usb_descriptors(NULL,18,iface,9,ep,7,&d));
    assert(!pw_xinput_usb_descriptors(dev,18,NULL,9,ep,7,&d));
    assert(!pw_xinput_usb_descriptors(dev,18,iface,9,NULL,7,&d));
    assert(!pw_xinput_usb_descriptors(dev,18,iface,9,ep,7,NULL));
    assert(pw_xinput_usb_report(report,20,&state));
    assert(state.buttons==0x9121 && state.left_trigger==255 && state.right_trigger==128);
    assert(state.thumb_lx==-32768 && state.thumb_ly==32767 && state.thumb_rx==-1 && state.thumb_ry==0x1234);
    /* Every raw button bit survives; no fret, strum or whammy remapping. */
    for(unsigned bit=0;bit<16;bit++) {
        report[2]=(uint8_t)(1u<<bit);report[3]=(uint8_t)((1u<<bit)>>8);
        assert(pw_xinput_usb_report(report,20,&state) && state.buttons==(1u<<bit));
    }
    for(size_t n=0;n<21;n++)if(n!=20)assert(!pw_xinput_usb_report(report,n,&state) && !state.buttons);
    assert(!pw_xinput_usb_report(report,21,&state));
    report[0]=1;assert(!pw_xinput_usb_report(report,20,&state));report[0]=0;
    report[1]=19;assert(!pw_xinput_usb_report(report,20,&state));
    assert(!pw_xinput_usb_report(NULL,20,&state) && !pw_xinput_usb_report(report,20,NULL));
    for(unsigned i=0;i<sizeof(xplorer_capture)/sizeof(xplorer_capture[0]);i++) {
        assert(pw_xinput_usb_report(xplorer_capture[i].data,20,&state));
        assert(state.buttons==xplorer_capture[i].buttons);
        assert(state.thumb_rx==xplorer_capture[i].whammy && state.thumb_ry==xplorer_capture[i].tilt);
    }
    return 0;
}
