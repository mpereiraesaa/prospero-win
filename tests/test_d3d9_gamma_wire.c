/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_gamma_wire.h"
#include <assert.h>
#include <string.h>
int main(void)
{
 unsigned char bytes[PW_D3D9_GAMMA_REQUEST_BYTES+1];struct pw_d3d9_gamma_request q={.method=PW_D3D9_GAMMA_SET,.swapchain=0xffffffffu,.flags=0x87654321u,.has_ramp=1},decoded;struct pw_d3d9_gamma_reply r={0},reply;
 for(unsigned i=0;i<PW_D3D9_GAMMA_WORDS;i++)q.ramp[i]=(uint16_t)(i*79);
 assert(!pw_d3d9_gamma_request_encode(bytes,PW_D3D9_GAMMA_REQUEST_BYTES,&q));assert(bytes[24+2]==79 && !bytes[24+3]);
 for(size_t n=0;n<PW_D3D9_GAMMA_REQUEST_BYTES;n++)assert(pw_d3d9_gamma_request_decode(&decoded,bytes,n));
 assert(pw_d3d9_gamma_request_decode(&decoded,bytes,sizeof(bytes)));assert(!pw_d3d9_gamma_request_decode(&decoded,bytes,PW_D3D9_GAMMA_REQUEST_BYTES));assert(!memcmp(&q,&decoded,sizeof(q)));
 bytes[20]=1;assert(pw_d3d9_gamma_request_decode(&decoded,bytes,PW_D3D9_GAMMA_REQUEST_BYTES));bytes[20]=0;
 r.method=q.method;assert(!pw_d3d9_gamma_reply_encode(bytes,PW_D3D9_GAMMA_REPLY_BYTES,&q,&r));r.has_ramp=1;assert(pw_d3d9_gamma_reply_encode(bytes,PW_D3D9_GAMMA_REPLY_BYTES,&q,&r));
 q.method=PW_D3D9_GAMMA_GET;q.flags=0;r.method=q.method;memcpy(r.ramp,q.ramp,sizeof(r.ramp));assert(!pw_d3d9_gamma_reply_encode(bytes,PW_D3D9_GAMMA_REPLY_BYTES,&q,&r));assert(!pw_d3d9_gamma_reply_decode(&reply,&q,bytes,PW_D3D9_GAMMA_REPLY_BYTES)&&!memcmp(reply.ramp,q.ramp,sizeof(q.ramp)));
 r.hresult=1;assert(pw_d3d9_gamma_reply_encode(bytes,PW_D3D9_GAMMA_REPLY_BYTES,&q,&r));r.hresult=0x80004005u;r.has_ramp=0;memset(r.ramp,0,sizeof(r.ramp));assert(!pw_d3d9_gamma_reply_encode(bytes,PW_D3D9_GAMMA_REPLY_BYTES,&q,&r));
 q.has_ramp=0;assert(pw_d3d9_gamma_request_encode(bytes,PW_D3D9_GAMMA_REQUEST_BYTES,&q));memset(q.ramp,0,sizeof(q.ramp));assert(!pw_d3d9_gamma_request_encode(bytes,PW_D3D9_GAMMA_REQUEST_BYTES,&q));return 0;
}
