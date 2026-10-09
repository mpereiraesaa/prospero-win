/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_gamma_wire.h"
#include <string.h>
static void put(uint8_t *p,uint32_t n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);p[2]=(uint8_t)(n>>16);p[3]=(uint8_t)(n>>24);}
static uint32_t get(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int zero(const uint16_t *r){for(unsigned i=0;i<PW_D3D9_GAMMA_WORDS;i++)if(r[i])return 0;return 1;}
static void put_ramp(uint8_t *p,const uint16_t *r){for(unsigned i=0;i<PW_D3D9_GAMMA_WORDS;i++){p[2*i]=(uint8_t)r[i];p[2*i+1]=(uint8_t)(r[i]>>8);}}
static void get_ramp(uint16_t *r,const uint8_t *p){for(unsigned i=0;i<PW_D3D9_GAMMA_WORDS;i++)r[i]=(uint16_t)((uint16_t)p[2*i]|(uint16_t)p[2*i+1]<<8);}
static int valid(const struct pw_d3d9_gamma_request *q)
{return q && (q->method==PW_D3D9_GAMMA_SET || q->method==PW_D3D9_GAMMA_GET) && q->has_ramp<=1 && (q->method!=PW_D3D9_GAMMA_GET || !q->flags) && (q->has_ramp || zero(q->ramp));}
int pw_d3d9_gamma_request_encode(void *out,size_t length,const struct pw_d3d9_gamma_request *q)
{
 uint8_t *p=out;if(!p || length!=PW_D3D9_GAMMA_REQUEST_BYTES || !valid(q))return -1;
 put(p,1);put(p+4,q->method);put(p+8,q->swapchain);put(p+12,q->flags);put(p+16,q->has_ramp);put(p+20,0);put_ramp(p+24,q->ramp);return 0;
}
int pw_d3d9_gamma_request_decode(struct pw_d3d9_gamma_request *q,const void *in,size_t length)
{
 const uint8_t *p=in;struct pw_d3d9_gamma_request t={0};if(!q || !p || length!=PW_D3D9_GAMMA_REQUEST_BYTES || get(p)!=1 || get(p+20))return -1;
 t.method=get(p+4);t.swapchain=get(p+8);t.flags=get(p+12);t.has_ramp=get(p+16);get_ramp(t.ramp,p+24);if(!valid(&t))return -1;*q=t;return 0;
}
static int valid_reply(const struct pw_d3d9_gamma_request *q,const struct pw_d3d9_gamma_reply *r)
{
 if(!valid(q) || !r || r->method!=q->method)return 0;
 if(r->hresult)return (r->hresult&0x80000000u) && !r->has_ramp && zero(r->ramp);
 return r->has_ramp==(q->method==PW_D3D9_GAMMA_GET?q->has_ramp:0) && (r->has_ramp || zero(r->ramp));
}
int pw_d3d9_gamma_reply_encode(void *out,size_t length,const struct pw_d3d9_gamma_request *q,const struct pw_d3d9_gamma_reply *r)
{
 uint8_t *p=out;if(!p || length!=PW_D3D9_GAMMA_REPLY_BYTES || !valid_reply(q,r))return -1;
 put(p,1);put(p+4,r->method);put(p+8,r->hresult);put(p+12,r->has_ramp);put_ramp(p+16,r->ramp);return 0;
}
int pw_d3d9_gamma_reply_decode(struct pw_d3d9_gamma_reply *r,const struct pw_d3d9_gamma_request *q,const void *in,size_t length)
{
 const uint8_t *p=in;struct pw_d3d9_gamma_reply t={0};if(!r || !p || length!=PW_D3D9_GAMMA_REPLY_BYTES || get(p)!=1)return -1;
 t.method=get(p+4);t.hresult=get(p+8);t.has_ramp=get(p+12);get_ramp(t.ramp,p+16);if(!valid_reply(q,&t))return -1;*r=t;return 0;
}
