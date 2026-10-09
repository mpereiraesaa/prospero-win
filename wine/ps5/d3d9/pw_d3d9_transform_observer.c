/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_transform_observer.h"
#include <string.h>
#define METHOD_SET_TRANSFORM 44u
#define METHOD_GET_TRANSFORM 45u
#define METHOD_MULTIPLY_TRANSFORM 46u
void pw_d3d9_transform_on_command(struct pw_d3d9_transform_shadow *s,const struct pw_d3d9_command *q,HRESULT hr)
{
    if(!s||!q)return;
    if(q->method==METHOD_SET_TRANSFORM){
        /* The proxy always sends 64 bytes; anything else is not a matrix we know. */
        if(q->data_bytes==64)pw_d3d9_transform_set(s,q->args[0],q->data.bytes,(uint32_t)hr);
        else pw_d3d9_transform_set(s,q->args[0],NULL,(uint32_t)E_FAIL);
    }else if(q->method==METHOD_MULTIPLY_TRANSFORM)pw_d3d9_transform_multiply(s,q->args[0]);
}
void pw_d3d9_transform_on_getter(struct pw_d3d9_transform_shadow *s,const struct pw_d3d9_getter_request *q,
 const struct pw_d3d9_getter_reply *r,HRESULT hr)
{
    if(!s||!q||q->method!=METHOD_GET_TRANSFORM||hr!=S_OK||!r)return;
    if(r->method==METHOD_GET_TRANSFORM&&r->hresult==(uint32_t)hr&&r->bytes==64)
        pw_d3d9_transform_observe(s,q->args[0],r->data.bytes,0);
}
void pw_d3d9_transform_on_stateblock(struct pw_d3d9_transform_shadow *s,uint32_t method,uint32_t type,
 struct pw_d3d9_stateblock_evidence *e,HRESULT hr)
{
    struct pw_d3d9_transform_block *b=e?&e->transform:NULL;
    if(!s)return;
    switch(method){
    case PW_D3D9_SB_BEGIN:pw_d3d9_transform_begin(s,(uint32_t)hr);break;
    case PW_D3D9_SB_END:pw_d3d9_transform_end(s,b,(uint32_t)hr);break;
    case PW_D3D9_SB_CREATE:pw_d3d9_transform_create(s,b,type,(uint32_t)hr);break;
    case PW_D3D9_SB_CAPTURE:pw_d3d9_transform_capture(s,b,(uint32_t)hr);break;
    case PW_D3D9_SB_APPLY:pw_d3d9_transform_apply(s,b,(uint32_t)hr);break;
    default: /* unknown operation: know nothing */
        pw_d3d9_transform_invalidate(s);pw_d3d9_transform_begin(s,(uint32_t)E_FAIL);break;
    }
}
void pw_d3d9_transform_on_reset(struct pw_d3d9_transform_shadow *s)
{pw_d3d9_transform_invalidate(s);}
int pw_d3d9_transform_answer(const struct pw_d3d9_transform_shadow *s,const struct pw_d3d9_getter_request *q,struct pw_d3d9_getter_reply *r)
{
    unsigned char value[64];
    if(!s||!q||!r||q->method!=METHOD_GET_TRANSFORM||!pw_d3d9_transform_lookup(s,q->args[0],value))return 0;
    memset(r,0,sizeof(*r));r->method=METHOD_GET_TRANSFORM;r->hresult=S_OK;r->bytes=64;memcpy(r->data.bytes,value,64);
    return 1;
}
void pw_d3d9_transform_invalidate_evidence(struct pw_d3d9_stateblock_evidence *e)
{if(e)e->transform.known=0;}
