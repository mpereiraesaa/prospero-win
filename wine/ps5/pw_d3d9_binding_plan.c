/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_binding_plan.h"
#include "d3d9/pw_d3d9_kinds.h"
int pw_d3d9_binding_plan(const struct pw_d3d9_command *c,struct pw_d3d9_binding *out)
{
    struct pw_d3d9_binding b={0};const struct pw_d3d9_command_schema *s;
    if(!c||!out)return PW_D3D9_BINDING_INVALID;
    b.method=c->method;
    switch(c->method){
    case 65:b.kind=PW_D3D9_KIND_TEXTURE_2D;b.object_word=1;break;
    case 87:b.kind=PW_D3D9_KIND_VERTEX_DECLARATION;break;
    case 92:b.kind=PW_D3D9_KIND_VERTEX_SHADER;break;
    case 100:b.kind=PW_D3D9_KIND_VERTEX_BUFFER;b.object_word=1;break;
    case 104:b.kind=PW_D3D9_KIND_INDEX_BUFFER;break;
    case 107:b.kind=PW_D3D9_KIND_PIXEL_SHADER;break;
    default:return PW_D3D9_BINDING_OTHER;
    }
    s=pw_d3d9_command_schema(c->method);
    if(!s||s->shape!=PW_D3D9_DATA_NONE||c->data_bytes||
       s->object_words!=(1u<<b.object_word)||s->words<=b.object_word+1)
        return PW_D3D9_BINDING_INVALID;
    for(unsigned i=s->words;i<PW_D3D9_COMMAND_WORDS;i++)
        if(c->args[i])return PW_D3D9_BINDING_INVALID;
    b.id=c->args[b.object_word];b.generation=c->args[b.object_word+1];
    if(!b.id!=!b.generation)return PW_D3D9_BINDING_INVALID;
    if(c->method==100&&c->args[0]>=16)return PW_D3D9_BINDING_INVALID;
    /* Native invalid-sampler calls are S_OK no-ops. Keep their current direct
     * path; this helper does not bypass the frontend's typed object resolver. */
    if(c->method==65&&!(c->args[0]<16||(c->args[0]>=256&&c->args[0]<=260)))
        return PW_D3D9_BINDING_SYNCHRONOUS;
    *out=b;return PW_D3D9_BINDING_READY;
}
