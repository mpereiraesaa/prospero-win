/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_vk_template_cache.h"
#include <string.h>
struct pw_vk_template_meta {
 struct pw_vk_template_meta *next;
 uint32_t device; uint64_t handle; size_t count;
 struct pw_vk_template_entry entries[];
};
static size_t template_bucket(uint32_t device,uint64_t handle)
{
 /* Mix both handle halves with the device using only 32-bit arithmetic, so
  * PE32 lookup does not need a 64-bit multiplication helper. */
 uint32_t high=(uint32_t)(handle>>32);
 uint32_t key=(uint32_t)handle ^ ((high<<13)|(high>>19)) ^ device*UINT32_C(0x9e3779b9);
 key^=key>>16;key*=UINT32_C(0x85ebca6b);
 key^=key>>13;key*=UINT32_C(0xc2b2ae35);key^=key>>16;
 return key & (PW_VK_TEMPLATE_BUCKETS-1u);
}
void pw_vk_template_remove(struct pw_vk_template_cache *c,const struct pw_vk_template_alloc *a,uint32_t d,uint64_t h)
{
 struct pw_vk_template_meta **p=&c->buckets[template_bucket(d,h)],*m;
 while((m=*p)) { if(m->device==d && m->handle==h) { *p=m->next;c->count--;a->free(m);return; } p=&m->next; }
}
int pw_vk_template_register(struct pw_vk_template_cache *c,const struct pw_vk_template_alloc *a,
 uint32_t d,uint64_t h,int ok,int pn,uint32_t flags,uint32_t type,const struct pw_vk_template_entry *e,size_t n)
{
 struct pw_vk_template_meta *m;size_t extent;
 if(!ok) return 0;
 /* Successful same-handle registration supersedes stale identity, including
  * when newly-created metadata is unsupported. No stale batch eligibility. */
 pw_vk_template_remove(c,a,d,h);
 if(!d || !h || pn || flags || type || !n || !e || n>(SIZE_MAX-sizeof(*m))/sizeof(*e)) return 0;
 if(!pw_vk_template_extent(e,n,&extent)) return 0;
 m=a->alloc(sizeof(*m)+n*sizeof(*e));if(!m)return 0;
 m->device=d;m->handle=h;m->count=n;memcpy(m->entries,e,n*sizeof(*e));
 {
  size_t bucket=template_bucket(d,h);
  m->next=c->buckets[bucket];c->buckets[bucket]=m;c->count++;
 }
 return 1;
}
const struct pw_vk_template_entry *pw_vk_template_lookup(const struct pw_vk_template_cache *c,uint32_t d,uint64_t h,size_t *n)
{
 const struct pw_vk_template_meta *m;*n=0;
 for(m=c->buckets[template_bucket(d,h)];m;m=m->next)if(m->device==d && m->handle==h){*n=m->count;return m->entries;}
 return NULL;
}
void pw_vk_template_remove_device(struct pw_vk_template_cache *c,const struct pw_vk_template_alloc *a,uint32_t d)
{
 size_t bucket;
 for(bucket=0;bucket<PW_VK_TEMPLATE_BUCKETS;bucket++) {
  struct pw_vk_template_meta **p=&c->buckets[bucket],*m;
  while((m=*p)){if(m->device==d){*p=m->next;c->count--;a->free(m);}else p=&m->next;}
 }
}

int pw_vk_template_snapshot(const struct pw_vk_template_cache *c,uint32_t d,uint64_t set,uint64_t h,
 const void *data,int guest32,void *out,size_t cap,size_t *written)
{
 size_t n,extent;uintptr_t base=(uintptr_t)data;
 const struct pw_vk_template_entry *e=pw_vk_template_lookup(c,d,h,&n);
 if(!e || !pw_vk_template_extent(e,n,&extent) || base>UINTPTR_MAX-extent) return 0;
 if(guest32 && (base>UINT32_MAX || extent>(uint64_t)UINT32_MAX+1-base)) return 0;
 return pw_vk_wire_template(out,cap,d,set,h,e,n,data,written);
}
