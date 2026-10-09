/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_command_policy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void check(struct pw_d3d9_command *c, int expected)
{
    struct pw_d3d9_command before = *c, decoded;
    unsigned char wire[PW_D3D9_COMMAND_MAX]; size_t written;
    assert(pw_d3d9_command_can_queue(c) == expected);
    assert(!memcmp(c, &before, sizeof(*c)));
    if (expected) {
        assert(!pw_d3d9_command_encode(wire, sizeof(wire), &written, c));
        assert(!pw_d3d9_command_decode(&decoded, wire, written));
        assert(pw_d3d9_command_can_queue(&decoded));
    }
}
int main(void)
{
    struct pw_d3d9_command c = {0};
    const unsigned copy_methods[] = {47,49,75}, sizes[] = {24,68,16};
    const unsigned indices[] = {0,1,6,7,255,256,0x7fffffff,0xffffffff};
    assert(!pw_d3d9_command_can_queue(NULL));
    for (unsigned i=0;i<3;i++) {
        c=(struct pw_d3d9_command){.method=copy_methods[i],.data_bytes=sizes[i]};
        memset(c.data.bytes,0xff,sizes[i]); check(&c,1);
        c.data_bytes--;check(&c,0);c.data_bytes+=2;check(&c,0);
        c.data_bytes=sizes[i];c.args[0]=1;check(&c,0);
    }
    for(unsigned i=0;i<sizeof(indices)/sizeof(indices[0]);i++) {
        c=(struct pw_d3d9_command){.method=57,.args={indices[i],0xffffffff}};check(&c,1);
        c.data_bytes=4;check(&c,0);c.data_bytes=0;c.args[2]=1;check(&c,0);
    }
    for(unsigned type=0;type<36;type++) {
        int valid=(type>=1&&type<=11)||(type>=22&&type<=24)||(type>=26&&type<=28)||type==32;
        c=(struct pw_d3d9_command){.method=67,.args={0xffffffff,type,0xffffffff}};check(&c,valid);
    }
    for(unsigned slot=0;slot<=261;slot++) for(unsigned type=0;type<=14;type++) {
        c=(struct pw_d3d9_command){.method=69,.args={slot,type,0xffffffff}};
        check(&c,(slot<16||(slot>=256&&slot<=260))&&type>=1&&type<=13);
    }
    c=(struct pw_d3d9_command){.method=69,.args={0xffffffff,1,0}};check(&c,0);
    c=(struct pw_d3d9_command){.method=67,.args={0,0xffffffff,0}};check(&c,0);
    /* Every other known schema remains synchronous even when well formed. */
    for(unsigned method=0;method<256;method++) {
        size_t bytes;
        if(method==47||method==49||method==57||method==67||method==69||method==75)continue;
        c=(struct pw_d3d9_command){.method=method};
        if(!pw_d3d9_command_data_bytes(method,c.args,&bytes))c.data_bytes=(uint32_t)bytes;
        check(&c,0);
    }
    puts("command eligibility policy passed");return 0;
}
