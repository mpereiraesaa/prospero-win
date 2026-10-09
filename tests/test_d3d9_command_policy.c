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
    /* Exact constant bounds, clamping, NULL and creation-mode behavior. */
    const unsigned constants[]={94,96,98,109,111,113};
    const unsigned flags[]={0x40,0x20,0x80,0xa0};
    for(unsigned m=0;m<6;m++)for(unsigned f=0;f<4;f++) {
        unsigned method=constants[m],vertex=m<3,floating=m==0||m==3;
        uint32_t software=vertex?(floating?8192u:2048u):(floating?224u:16u);
        uint32_t hardware=vertex&&!(flags[f]&0xa0u)?(floating?256u:16u):software;
        uint32_t effective=999;
        assert(pw_d3d9_command_constant_count(method,0,0,flags[f],0,&effective)==1&&effective==0);
        assert(pw_d3d9_command_constant_count(method,software,0,flags[f],0,&effective)==1&&effective==0);
        effective=999;assert(pw_d3d9_command_constant_count(method,software+1,0,flags[f],0,&effective)==-1&&effective==999);
        assert(pw_d3d9_command_constant_count(method,UINT32_MAX,1,flags[f],1,&effective)==-1&&effective==999);
        assert(pw_d3d9_command_constant_count(method,0,1,flags[f],0,&effective)==-1&&effective==999);
        assert(pw_d3d9_command_constant_count(method,hardware,software-hardware,flags[f],0,&effective)==1&&effective==0);
        assert(pw_d3d9_command_constant_count(method,hardware-1,software-hardware+1,flags[f],1,&effective)==1&&effective==1);
        assert(pw_d3d9_command_constant_count(method,0,software,flags[f],1,&effective)==1&&effective==hardware);
        c=(struct pw_d3d9_command){.method=method,.args={software,0}};check(&c,1);
        c.args[0]++;check(&c,0);
        c=(struct pw_d3d9_command){.method=method,.args={0,1},.data_bytes=(method==98||method==113)?4:16};check(&c,1);
        if(method==98||method==113){c.data.words[0]=2;check(&c,0);}
    }
    for(unsigned state=0;state<520;state++) {
        c=(struct pw_d3d9_command){.method=44,.args={state},.data_bytes=64};
        check(&c,state==2||state==3||(state>=16&&state<=23)||(state>=256&&state<=511));
    }
    /* Every other known schema remains synchronous even when well formed. */
    for(unsigned method=0;method<256;method++) {
        size_t bytes;
        if(method==44||method==47||method==49||method==57||method==67||method==69||method==75||method==94||method==96||method==98||method==109||method==111||method==113)continue;
        c=(struct pw_d3d9_command){.method=method};
        if(!pw_d3d9_command_data_bytes(method,c.args,&bytes))c.data_bytes=(uint32_t)bytes;
        check(&c,0);
    }
    puts("command eligibility policy passed");return 0;
}
