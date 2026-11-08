/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_XINPUT_NET_H
#define PW_XINPUT_NET_H
#include <stddef.h>
#include <stdint.h>

/* XInput controllers another process on the console describes in small
 * UDP messages to 127.0.0.1 (a Bluetooth service such as PadBridge PS5 with
 * a Wii Remote guitar). One fixed little-endian message per state:
 *
 *    0  "PBXI"            4  u8 version 1        5  u8 source index 0..3
 *    6  u8 flags: bit 0 connected, the rest 0     7  u8 XINPUT_DEVSUBTYPE
 *    8  u16 vendor id    10  u16 product id      12  u16 revision
 *   14  u16 XINPUT_GAMEPAD buttons               16  u8 left, 17 u8 right trigger
 *   18  i16 lx  20 i16 ly  22 i16 rx  24 i16 ry  26  u32 sequence  30  u16 0
 *
 * A source is connected while its messages keep coming (the sender repeats
 * a still state every 100 ms) and gone after PW_XINPUT_NET_TIMEOUT_MS
 * without one, or at once on a message without the connected flag. */
enum { PW_XINPUT_NET_SIZE=32, PW_XINPUT_NET_PORT=8097, PW_XINPUT_NET_SOURCES=4,
       PW_XINPUT_NET_TIMEOUT_MS=500, PW_XINPUT_NET_SLOTS=4 };

typedef struct PwXinputNetMessage {
    uint8_t index,connected,subtype;
    uint16_t vendor_id,product_id,revision,buttons;
    uint8_t left_trigger,right_trigger;
    int16_t thumb_lx,thumb_ly,thumb_rx,thumb_ry;
    uint32_t sequence;
} PwXinputNetMessage;

/* 0 and the message, or -1: wrong size, magic, version, index, flags,
 * subtype or reserved bytes. Buttons keep only XINPUT_GAMEPAD's bits. */
int pw_xinput_net_parse(const uint8_t *data,size_t length,PwXinputNetMessage *out);

/* Which XInput slot each source holds. Slot 0 is the console user's
 * DualSense and never given; a source takes the highest slot that is free
 * (busy: another device, such as a wired USB guitar, holds it), and keeps
 * it until it is gone. */
typedef struct PwXinputNetSource { uint8_t active; uint32_t slot; uint64_t last_ms; } PwXinputNetSource;
typedef struct PwXinputNet { PwXinputNetSource source[PW_XINPUT_NET_SOURCES]; } PwXinputNet;

enum { PW_XINPUT_NET_IGNORE, PW_XINPUT_NET_SET, PW_XINPUT_NET_CLEAR };

void pw_xinput_net_init(PwXinputNet *net);
/* What a valid message means: SET the slot to its state, CLEAR the slot, or
 * IGNORE it (gone and not held, or no slot free). busy[i] != 0: slot i is
 * held by something else. */
int pw_xinput_net_accept(PwXinputNet *net,const PwXinputNetMessage *message,uint64_t now_ms,
                         const uint8_t busy[PW_XINPUT_NET_SLOTS],uint32_t *slot);
/* 1, the slot and the source index of a source silent for too long, which
 * is then gone; 0 when none is. Called until 0. */
int pw_xinput_net_expire(PwXinputNet *net,uint64_t now_ms,uint32_t *slot,unsigned *index);
/* 1 when a slot is held by a source. */
int pw_xinput_net_holds(const PwXinputNet *net,uint32_t slot);
#endif
