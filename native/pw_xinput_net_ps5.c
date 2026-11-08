/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_xinput_net_wine.h"
#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>

/* A non-blocking UDP socket bound to 127.0.0.1 only: nothing off the
 * console reaches it. sceNet, as the Lapy helper's loopback connection. */
struct pw_xinput_net_sockaddr_in {
    uint8_t length;
    uint8_t family;
    uint16_t port;
    uint32_t address;
    uint16_t virtual_port;
    uint8_t zero[6];
};

extern int sceNetSocket(const char *name,int family,int type,int protocol);
extern int sceNetBind(int socket,const struct sockaddr *address,socklen_t length);
extern int sceNetRecv(int socket,void *data,size_t length,int flags);
extern int sceNetSetsockopt(int socket,int level,int option,const void *value,socklen_t length);
extern int sceNetSocketClose(int socket);
extern int *sceNetErrnoLoc(void);

enum { PW_SCE_SO_NBIO=0x1200 };

static int net_errno(void)
{
    int *e=sceNetErrnoLoc();
    return e && *e ? *e : EIO;
}

static int ps5_open(void *ctx,uint16_t port)
{
    struct pw_xinput_net_sockaddr_in address={0};
    int on=1,fd,e;
    (void)ctx;
    fd=sceNetSocket("pwxinput",AF_INET,SOCK_DGRAM,0);
    if(fd<0)return -net_errno();
    if(sceNetSetsockopt(fd,SOL_SOCKET,PW_SCE_SO_NBIO,&on,sizeof(on))<0)goto failed;
    address.length=sizeof(address);
    address.family=AF_INET;
    address.port=(uint16_t)((port<<8)|(port>>8));
    address.address=UINT32_C(0x0100007f);
    if(sceNetBind(fd,(const struct sockaddr *)&address,sizeof(address))<0)goto failed;
    return fd;
failed:
    e=net_errno();
    sceNetSocketClose(fd);
    return -e;
}

static long ps5_receive(void *ctx,int fd,uint8_t *data,size_t cap)
{
    int n;
    (void)ctx;
    n=sceNetRecv(fd,data,cap,0);
    if(n>=0)return n;
    n=net_errno();
    return n==EAGAIN || n==EWOULDBLOCK ? 0 : -n;
}

static void ps5_close(void *ctx,int fd)
{
    (void)ctx;
    sceNetSocketClose(fd);
}

const PwXinputNetOps *pw_xinput_net_ps5_ops(void)
{
    static const PwXinputNetOps ops={ps5_open,ps5_receive,ps5_close,NULL};
    return &ops;
}
