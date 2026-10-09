/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_implicit_wire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void request_bad(const unsigned char *bytes, size_t n)
{
    struct pw_d3d9_implicit_request output, before;
    memset(&output,0xa5,sizeof(output)); before=output;
    assert(pw_d3d9_implicit_request_decode(&output,bytes,n));
    assert(!memcmp(&output,&before,sizeof(output)));
}
static void reply_bad(const struct pw_d3d9_implicit_request *q,const unsigned char *bytes,size_t n)
{
    struct pw_d3d9_implicit_reply output,before;
    memset(&output,0xa5,sizeof(output)); before=output;
    assert(pw_d3d9_implicit_reply_decode(&output,q,bytes,n));
    assert(!memcmp(&output,&before,sizeof(output)));
}
int main(void)
{
    unsigned char in[PW_D3D9_IMPLICIT_REQUEST_BYTES+1],out[PW_D3D9_IMPLICIT_REPLY_BYTES+1],copy[sizeof(out)];
    struct pw_d3d9_implicit_request q={.operation=PW_D3D9_IMPLICIT_PREPARE},decoded;
    struct pw_d3d9_implicit_reply r={0},rd;
    for (unsigned n=0;n<=PW_D3D9_IMPLICIT_MAX;n++) {
        q.count=n;
        for (unsigned i=0;i<n;i++) q.objects[i]=(struct pw_d3d9_object_ref){i+1,0xfedcba98u};
        assert(!pw_d3d9_implicit_request_encode(in,sizeof(in)-1,&q));
        assert(in[0]==1 && in[4]==2 && in[8]==n);
        if(n) assert(in[16]==1 && in[20]==0x98 && in[23]==0xfe);
        assert(!pw_d3d9_implicit_request_decode(&decoded,in,sizeof(in)-1));
        assert(!memcmp(&q,&decoded,sizeof(q)));
    }
    for(size_t n=0;n<sizeof(in)-1;n++) request_bad(in,n);
    request_bad(in,sizeof(in)); request_bad(NULL,sizeof(in)-1);
    const unsigned bad_offsets[]={0,4,8,12,16,20};
    for(unsigned i=0;i<sizeof(bad_offsets)/sizeof(bad_offsets[0]);i++) {
        memcpy(copy,in,sizeof(in)-1); memset(copy+bad_offsets[i],0,4);
        if(bad_offsets[i]==12) copy[12]=1;
        request_bad(copy,sizeof(in)-1);
    }
    memcpy(copy,in,sizeof(in)-1); copy[8]=17; request_bad(copy,sizeof(in)-1);
    memcpy(copy,in,sizeof(in)-1); memcpy(copy+24,copy+16,4); request_bad(copy,sizeof(in)-1);
    q=(struct pw_d3d9_implicit_request){.operation=PW_D3D9_IMPLICIT_LIST};
    assert(!pw_d3d9_implicit_request_encode(in,sizeof(in)-1,&q));
    memcpy(copy,in,sizeof(in)-1);copy[16]=1;request_bad(copy,sizeof(in)-1);
    q.count=1;q.objects[0]=(struct pw_d3d9_object_ref){1,2};
    memset(in,0xa5,sizeof(in));memcpy(copy,in,sizeof(in));
    assert(pw_d3d9_implicit_request_encode(in,sizeof(in)-1,&q));
    assert(!memcmp(in,copy,sizeof(in)));
    for(unsigned op=PW_D3D9_IMPLICIT_LIST;op<=PW_D3D9_IMPLICIT_DRAIN;op++) {
        q=(struct pw_d3d9_implicit_request){.operation=op};
        r=(struct pw_d3d9_implicit_reply){.operation=op};
        if(op==PW_D3D9_IMPLICIT_FINISH) r.disposition=PW_D3D9_IMPLICIT_RESTORED;
        if(op==PW_D3D9_IMPLICIT_LIST || op==PW_D3D9_IMPLICIT_FINISH) {
            r.count=16;
            for(unsigned i=0;i<16;i++)r.objects[i]=(struct pw_d3d9_object_ref){i+1,UINT32_MAX};
        }
        assert(!pw_d3d9_implicit_reply_encode(out,sizeof(out)-1,&q,&r));
        assert(!pw_d3d9_implicit_reply_decode(&rd,&q,out,sizeof(out)-1));
        assert(!memcmp(&r,&rd,sizeof(r)));
        for(size_t n=0;n<sizeof(out)-1;n++)reply_bad(&q,out,n);
        reply_bad(&q,out,sizeof(out));reply_bad(&q,NULL,sizeof(out)-1);
        memcpy(copy,out,sizeof(out)-1);copy[20]=1;reply_bad(&q,copy,sizeof(out)-1);
        memcpy(copy,out,sizeof(out)-1);copy[4]=9;reply_bad(&q,copy,sizeof(out)-1);
        memcpy(copy,out,sizeof(out)-1);copy[12]=9;reply_bad(&q,copy,sizeof(out)-1);
        memcpy(copy,out,sizeof(out)-1);copy[16]=17;reply_bad(&q,copy,sizeof(out)-1);
        r=(struct pw_d3d9_implicit_reply){.operation=op,.hresult=0x8876086cu};
        assert(!pw_d3d9_implicit_reply_encode(out,sizeof(out)-1,&q,&r));
        assert(!pw_d3d9_implicit_reply_decode(&rd,&q,out,sizeof(out)-1));
        memcpy(copy,out,sizeof(out)-1);copy[12]=1;reply_bad(&q,copy,sizeof(out)-1);
        memcpy(copy,out,sizeof(out)-1);copy[24]=1;reply_bad(&q,copy,sizeof(out)-1);
        r.hresult=1;assert(pw_d3d9_implicit_reply_encode(out,sizeof(out)-1,&q,&r));
    }
    q=(struct pw_d3d9_implicit_request){.operation=PW_D3D9_IMPLICIT_FINISH};
    r=(struct pw_d3d9_implicit_reply){.operation=q.operation,.disposition=PW_D3D9_IMPLICIT_RETIRED};
    assert(!pw_d3d9_implicit_reply_encode(out,sizeof(out)-1,&q,&r));
    assert(!pw_d3d9_implicit_reply_decode(&rd,&q,out,sizeof(out)-1));
    r.disposition=0;assert(pw_d3d9_implicit_reply_encode(out,sizeof(out)-1,&q,&r));
    assert(pw_d3d9_implicit_request_encode(NULL,sizeof(in)-1,&q));
    assert(pw_d3d9_implicit_reply_encode(NULL,sizeof(out)-1,&q,&r));
    puts("D3D9 implicit-owner wire: PASS");return 0;
}
