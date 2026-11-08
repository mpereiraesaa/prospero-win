/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_XINPUT_USB_H
#define PW_XINPUT_USB_H
#include <stddef.h>
#include <stdint.h>

enum { PW_XINPUT_USB_REPORT=20, PW_XINPUT_USB_PACKET_MAX=64 };
typedef struct PwXinputUsbDevice {
    uint16_t vendor_id,product_id,revision,max_packet;
    uint8_t subtype,interface_number,endpoint_address;
} PwXinputUsbDevice;
typedef struct PwXinputUsbState {
    uint16_t buttons;
    uint8_t left_trigger,right_trigger;
    int16_t thumb_lx,thumb_ly,thumb_rx,thumb_ry;
} PwXinputUsbState;
/* Cached standard USB descriptors; accepts only alt-0 FF/5D/01 and an
 * interrupt-IN endpoint large enough for a wired 360 input report. */
int pw_xinput_usb_descriptors(const uint8_t *device,size_t device_len,
                              const uint8_t *interface,size_t interface_len,
                              const uint8_t *endpoint,size_t endpoint_len,
                              PwXinputUsbDevice *out);
/* One complete USB transfer, not a byte stream. Status/output reports,
 * short/truncated/oversized input and invalid headers are rejected. */
int pw_xinput_usb_report(const uint8_t *data,size_t length,PwXinputUsbState *out);
#endif
