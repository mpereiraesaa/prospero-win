/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_implicit_wire.h"
static void put(uint8_t *p, uint32_t n)
{ p[0]=(uint8_t)n; p[1]=(uint8_t)(n>>8); p[2]=(uint8_t)(n>>16); p[3]=(uint8_t)(n>>24); }
static uint32_t get(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static int refs_valid(const struct pw_d3d9_object_ref *refs, uint32_t count)
{
    if (count > PW_D3D9_IMPLICIT_MAX) return 0;
    for (uint32_t i=0; i<PW_D3D9_IMPLICIT_MAX; ++i) {
        if (i>=count) { if (refs[i].id || refs[i].generation) return 0; }
        else {
            if (!refs[i].id || !refs[i].generation) return 0;
            /* An ID can have only one live generation within a transaction. */
            for (uint32_t j=0; j<i; ++j) if (refs[i].id==refs[j].id) return 0;
        }
    }
    return 1;
}
static int valid(const struct pw_d3d9_implicit_request *q)
{
    return q && q->operation>=PW_D3D9_IMPLICIT_LIST && q->operation<=PW_D3D9_IMPLICIT_DRAIN &&
        (q->operation==PW_D3D9_IMPLICIT_PREPARE || !q->count) && refs_valid(q->objects,q->count);
}
static void refs_put(uint8_t *p, const struct pw_d3d9_object_ref *refs)
{ for (uint32_t i=0;i<PW_D3D9_IMPLICIT_MAX;++i) { put(p+8*i,refs[i].id); put(p+8*i+4,refs[i].generation); } }
static void refs_get(struct pw_d3d9_object_ref *refs, const uint8_t *p)
{ for (uint32_t i=0;i<PW_D3D9_IMPLICIT_MAX;++i) { refs[i].id=get(p+8*i); refs[i].generation=get(p+8*i+4); } }
int pw_d3d9_implicit_request_encode(void *out, size_t length, const struct pw_d3d9_implicit_request *q)
{
    uint8_t *p=out;
    if (!p || length!=PW_D3D9_IMPLICIT_REQUEST_BYTES || !valid(q)) return -1;
    put(p,1); put(p+4,q->operation); put(p+8,q->count); put(p+12,0); refs_put(p+16,q->objects);
    return 0;
}
int pw_d3d9_implicit_request_decode(struct pw_d3d9_implicit_request *q, const void *in, size_t length)
{
    const uint8_t *p=in; struct pw_d3d9_implicit_request t={0};
    if (!q || !p || length!=PW_D3D9_IMPLICIT_REQUEST_BYTES || get(p)!=1 || get(p+12)) return -1;
    t.operation=get(p+4); t.count=get(p+8); refs_get(t.objects,p+16);
    if (!valid(&t)) return -1;
    *q=t; return 0;
}
static int valid_reply(const struct pw_d3d9_implicit_request *q, const struct pw_d3d9_implicit_reply *r)
{
    if (!valid(q) || !r || r->operation!=q->operation || !refs_valid(r->objects,r->count)) return 0;
    if (r->hresult) return (r->hresult&0x80000000u) && !r->count && !r->disposition;
    if (q->operation==PW_D3D9_IMPLICIT_FINISH)
        return r->disposition==PW_D3D9_IMPLICIT_RESTORED || r->disposition==PW_D3D9_IMPLICIT_RETIRED;
    return !r->disposition && (q->operation==PW_D3D9_IMPLICIT_LIST || !r->count);
}
int pw_d3d9_implicit_reply_encode(void *out, size_t length, const struct pw_d3d9_implicit_request *q, const struct pw_d3d9_implicit_reply *r)
{
    uint8_t *p=out;
    if (!p || length!=PW_D3D9_IMPLICIT_REPLY_BYTES || !valid_reply(q,r)) return -1;
    put(p,1); put(p+4,r->operation); put(p+8,r->hresult); put(p+12,r->disposition); put(p+16,r->count); put(p+20,0); refs_put(p+24,r->objects);
    return 0;
}
int pw_d3d9_implicit_reply_decode(struct pw_d3d9_implicit_reply *r, const struct pw_d3d9_implicit_request *q, const void *in, size_t length)
{
    const uint8_t *p=in; struct pw_d3d9_implicit_reply t={0};
    if (!r || !p || length!=PW_D3D9_IMPLICIT_REPLY_BYTES || get(p)!=1 || get(p+20)) return -1;
    t.operation=get(p+4); t.hresult=get(p+8); t.disposition=get(p+12); t.count=get(p+16); refs_get(t.objects,p+24);
    if (!valid_reply(q,&t)) return -1;
    *r=t; return 0;
}
