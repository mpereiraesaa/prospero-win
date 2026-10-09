/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Cross-architecture record exchange. Build as PE32 and as native Unix64. */
#include "wine/ps5/pw_d3d9_bridge_wire.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define TICKET UINT64_C(0x1234567800000021)
static _Alignas(8) unsigned char arena[384];
static struct pw_d3d9_channel client,service;
static struct pw_d3d9_message request(void)
{
    return (struct pw_d3d9_message){.opcode=PW_D3D9_FACTORY_CALL,.device=0xabcdef01,
        .object=0x12345678,.generation=0x87654321,.payload_bytes=17};
}
static void prepare(void)
{
    struct pw_d3d9_message hello={.opcode=PW_D3D9_HELLO},r;unsigned char scratch[128];
    assert(pw_d3d9_channel_init(arena,sizeof(arena),7,128,128)==PW_D3D9_OK);
    assert(pw_d3d9_channel_open(&client,arena,sizeof(arena),7,PW_D3D9_CLIENT)==PW_D3D9_OK);
    assert(pw_d3d9_channel_open(&service,arena,sizeof(arena),7,PW_D3D9_SERVICE)==PW_D3D9_OK);
    assert(pw_d3d9_channel_send(&client,&hello,NULL)==PW_D3D9_OK);
    assert(pw_d3d9_channel_receive(&service,&r,scratch,sizeof(scratch))==PW_D3D9_OK);
    assert(pw_d3d9_channel_ready(&service)==PW_D3D9_OK);r.sequence=0;
    assert(pw_d3d9_channel_send(&service,&r,NULL)==PW_D3D9_OK);
    assert(pw_d3d9_channel_receive(&client,&r,scratch,sizeof(scratch))==PW_D3D9_OK);
    client.next_send=client.next_receive=service.next_send=service.next_receive=TICKET;
}
static void file(const char *path,int write)
{
    FILE *f=fopen(path,write?"wb":"rb");assert(f);
    if(write)assert(fwrite(arena,1,sizeof(arena),f)==sizeof(arena));
    else{assert(fread(arena,1,sizeof(arena),f)==sizeof(arena));assert(fgetc(f)==EOF);}
    assert(!fclose(f));
}
static void verify(const struct pw_d3d9_message *m,const unsigned char *scratch,int reply)
{
    struct pw_d3d9_message expected=request();
    assert(m->sequence==TICKET && m->ticket==TICKET && m->opcode==expected.opcode);
    assert(m->device==expected.device && m->object==expected.object && m->generation==expected.generation);
    assert(m->payload_bytes==17 && m->result==(reply?(int32_t)0x8876086c:0));
    for(unsigned i=0;i<17;i++)assert(scratch[64+i]==(unsigned char)(i*13+7));
}
int main(int argc,char **argv)
{
    unsigned char payload[17],scratch[128];struct pw_d3d9_message m=request(),r;
    if(argc<3)return 2;
    prepare();
    if(!strcmp(argv[1],"produce")) {
        for(unsigned i=0;i<17;i++)payload[i]=(unsigned char)(i*13+7);
        assert(pw_d3d9_channel_send(&client,&m,payload)==PW_D3D9_OK);memset(payload,0,sizeof(payload));file(argv[2],1);
    } else if(!strcmp(argv[1],"consume") && argc==4) {
        file(argv[2],0);assert(pw_d3d9_channel_receive(&service,&r,scratch,sizeof(scratch))==PW_D3D9_OK);verify(&r,scratch,0);
        r.sequence=0;r.result=(int32_t)0x8876086c;
        assert(pw_d3d9_channel_send(&service,&r,scratch+64)==PW_D3D9_OK);file(argv[3],1);
    } else if(!strcmp(argv[1],"check")) {
        file(argv[2],0);client.pending_count=1;client.pending[(TICKET-1)%PW_D3D9_WIRE_PENDING]=m;
        assert(pw_d3d9_channel_receive(&client,&r,scratch,sizeof(scratch))==PW_D3D9_OK);verify(&r,scratch,1);
    } else return 2;
    printf("D3D9 wire %u-bit %s passed\n",(unsigned)(sizeof(void *)*8),argv[1]);return 0;
}
