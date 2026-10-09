/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_stateblock_wire.h"
#include <string.h>
static uint32_t get32(const unsigned char *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(unsigned char *p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
int pw_d3d9_stateblock_validate(const struct pw_d3d9_stateblock_request *q)
{
 if(!q)return PW_D3D9_SB_INVALID;
 switch(q->method){
 case PW_D3D9_SB_CREATE:return PW_D3D9_SB_OK;
 case PW_D3D9_SB_BEGIN:case PW_D3D9_SB_END:case PW_D3D9_SB_CAPTURE:case PW_D3D9_SB_APPLY:
  return q->type?PW_D3D9_SB_INVALID:PW_D3D9_SB_OK;
 default:return PW_D3D9_SB_UNSUPPORTED;
 }
}
int pw_d3d9_stateblock_encode(void *output,size_t bytes,const struct pw_d3d9_stateblock_request *q)
{
 unsigned char wire[16];int status=pw_d3d9_stateblock_validate(q);
 if(status)return status;
 if(!output || bytes!=16)return PW_D3D9_SB_INVALID;
 put32(wire,1);put32(wire+4,q->method);put32(wire+8,q->type);put32(wire+12,0);memcpy(output,wire,16);return 0;
}
int pw_d3d9_stateblock_decode(struct pw_d3d9_stateblock_request *out,const void *input,size_t bytes)
{
 const unsigned char *p=input;struct pw_d3d9_stateblock_request q;int status;
 if(!out || !input || bytes!=16 || get32(p)!=1 || get32(p+12))return PW_D3D9_SB_INVALID;
 q=(struct pw_d3d9_stateblock_request){get32(p+4),get32(p+8)};
 if((status=pw_d3d9_stateblock_validate(&q)))return status;
 *out=q;return 0;
}
static int reply_valid(const struct pw_d3d9_stateblock_request *q,const struct pw_d3d9_stateblock_reply *r)
{
 int status=pw_d3d9_stateblock_validate(q);int creates;
 if(status)return status;
 if(!r)return PW_D3D9_SB_INVALID;
 creates=(q->method==PW_D3D9_SB_CREATE || q->method==PW_D3D9_SB_END) && !(r->hresult&0x80000000u);
 if(creates)return r->object.id && r->object.generation?0:PW_D3D9_SB_INVALID;
 return r->object.id || r->object.generation?PW_D3D9_SB_INVALID:0;
}
int pw_d3d9_stateblock_reply_encode(void *output,size_t bytes,const struct pw_d3d9_stateblock_request *q,const struct pw_d3d9_stateblock_reply *r)
{
 unsigned char wire[16];int status=reply_valid(q,r);
 if(status)return status;
 if(!output || bytes!=16)return PW_D3D9_SB_INVALID;
 put32(wire,1);put32(wire+4,r->hresult);put32(wire+8,r->object.id);put32(wire+12,r->object.generation);
 memcpy(output,wire,16);return 0;
}
int pw_d3d9_stateblock_reply_decode(struct pw_d3d9_stateblock_reply *out,const struct pw_d3d9_stateblock_request *q,const void *input,size_t bytes)
{
 const unsigned char *p=input;struct pw_d3d9_stateblock_reply r;int status;
 if(!out || !input || bytes!=16 || get32(p)!=1)return PW_D3D9_SB_INVALID;
 r=(struct pw_d3d9_stateblock_reply){get32(p+4),{get32(p+8),get32(p+12)}};
 if((status=reply_valid(q,&r)))return status;
 *out=r;return 0;
}
