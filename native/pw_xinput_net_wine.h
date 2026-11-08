/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_XINPUT_NET_WINE_H
#define PW_XINPUT_NET_WINE_H
#include "../src/pw_xinput_net.h"
#include "../wine/ps5/pw_wine_sink.h"

/* XInput controllers described over loopback UDP (src/pw_xinput_net.h)
 * as XInput users 1..3, beside the title's DualSense (user 0), the other
 * signed-in users' DualSenses and wired USB devices. The socket is the
 * platform's (pw_xinput_net_ps5.c); tests give their own. */
typedef struct PwXinputNetOps {
    int (*open)(void *ctx,uint16_t port);                    /* fd, or -errno */
    long (*receive)(void *ctx,int fd,uint8_t *data,size_t cap); /* bytes, 0: none waiting, or -errno */
    void (*close)(void *ctx,int fd);
    void *ctx;
} PwXinputNetOps;
/* pw_wine_set_pad_device_at: state and identity at once, NULL disconnects. */
typedef int (*PwXinputNetSetSlot)(uint32_t,const PwWinePad *,const PwWinePadCaps *);

typedef struct PwXinputNetWine {
    PwXinputNet net;
    PwXinputNetOps ops;
    int fd;
    PwXinputNetSetSlot set_slot;
    void (*log)(const char *);
    uint32_t reserved;
    unsigned blocked;
    PwWinePad pad[PW_XINPUT_NET_SOURCES];
    PwWinePadCaps caps[PW_XINPUT_NET_SOURCES];
    uint64_t received,rejected;
} PwXinputNetWine;

/* Listens when the device sink is there; 0 when listening or disabled
 * (no sink), else the socket's -errno, logged, and the bridge stays off. */
int pw_xinput_net_wine_init(PwXinputNetWine *bridge,const PwXinputNetOps *ops,
                            PwXinputNetSetSlot set_slot,void (*log)(const char *));
/* The users others hold (bit i: user i; user 0 always). A source on one of
 * them is disconnected there at once and takes a free user with its next
 * message, which comes within 100 ms. */
void pw_xinput_net_wine_reserve(PwXinputNetWine *bridge,uint32_t users);
/* While blocked (the title's menu is open) every source reads idle. */
void pw_xinput_net_wine_block(PwXinputNetWine *bridge,int blocked);
/* The users the bridge's sources hold now. */
uint32_t pw_xinput_net_wine_users(const PwXinputNetWine *bridge);
/* Takes what arrived (a bounded number per call) and lets silent sources go. */
void pw_xinput_net_wine_poll(PwXinputNetWine *bridge,uint64_t now_ms);
/* Every held user disconnected, the socket closed. */
void pw_xinput_net_wine_close(PwXinputNetWine *bridge);

/* The console's loopback UDP socket (sceNet). */
const PwXinputNetOps *pw_xinput_net_ps5_ops(void);
#endif
