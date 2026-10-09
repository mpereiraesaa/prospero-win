/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_window.h"
#include <limits.h>
#include <string.h>
static int equal(struct pw_d3d9_window_id a,struct pw_d3d9_window_id b)
{return a.epoch==b.epoch && a.id==b.id && a.generation==b.generation;}
static struct pw_d3d9_window_entry *entry(struct pw_d3d9_windows *r,struct pw_d3d9_window_id id)
{
 struct pw_d3d9_window_entry *e;
 if(!r || !r->epoch || id.epoch!=r->epoch || !id.id || id.id>PW_D3D9_WINDOWS || !id.generation)return NULL;
 e=&r->windows[id.id-1];return e->live && e->generation==id.generation?e:NULL;
}
static int valid_state(const struct pw_d3d9_window_state *s)
{
 return s && !(s->flags&~7u) && s->width<=INT32_MAX && s->height<=INT32_MAX &&
  (int64_t)s->x+s->width<=INT32_MAX && (int64_t)s->y+s->height<=INT32_MAX;
}
int pw_d3d9_windows_init(struct pw_d3d9_windows *r,uint32_t epoch)
{
 if(!r || !epoch)return PW_D3D9_WINDOW_INVALID;
 memset(r,0,sizeof(*r));r->epoch=epoch;return PW_D3D9_WINDOW_OK;
}
int pw_d3d9_window_attach(struct pw_d3d9_windows *r,uint64_t guest,uint64_t service,struct pw_d3d9_window_id *id)
{
 unsigned i;struct pw_d3d9_window_entry *e;
 if(!r || !r->epoch || !guest || !service || guest==service || !id)return PW_D3D9_WINDOW_INVALID;
 for(i=0;i<PW_D3D9_WINDOWS;i++)if(r->windows[i].live &&
  (r->windows[i].guest==guest || r->windows[i].service==guest || r->windows[i].guest==service || r->windows[i].service==service))return PW_D3D9_WINDOW_BUSY;
 for(i=0;i<PW_D3D9_WINDOWS;i++)if(!r->windows[i].live && r->windows[i].generation<UINT32_MAX){
  uint32_t generation=r->windows[i].generation+1;e=&r->windows[i];memset(e,0,sizeof(*e));
  e->generation=generation;e->guest=guest;e->service=service;e->live=1;
  id->epoch=r->epoch;id->id=i+1;id->generation=generation;return PW_D3D9_WINDOW_OK;
 }
 return PW_D3D9_WINDOW_EXHAUSTED;
}
int pw_d3d9_window_find(const struct pw_d3d9_windows *r,uint64_t handle,struct pw_d3d9_window_id *id)
{
 unsigned i;if(!r || !handle || !id)return PW_D3D9_WINDOW_INVALID;
 for(i=0;i<PW_D3D9_WINDOWS;i++)if(r->windows[i].live && (r->windows[i].guest==handle || r->windows[i].service==handle)){
  id->epoch=r->epoch;id->id=i+1;id->generation=r->windows[i].generation;return PW_D3D9_WINDOW_OK;
 }
 return PW_D3D9_WINDOW_STALE;
}
int pw_d3d9_window_get(const struct pw_d3d9_windows *r,struct pw_d3d9_window_id id,struct pw_d3d9_window_entry *out)
{
 struct pw_d3d9_window_entry *e=entry((struct pw_d3d9_windows *)r,id);
 if(!out)return PW_D3D9_WINDOW_INVALID;
 if(!e)return PW_D3D9_WINDOW_STALE;
 *out=*e;return PW_D3D9_WINDOW_OK;
}
int pw_d3d9_window_begin(struct pw_d3d9_windows *r,struct pw_d3d9_window_id id,uint64_t seq,const struct pw_d3d9_window_state *state)
{
 struct pw_d3d9_window_entry *e=entry(r,id);
 if(!e)return PW_D3D9_WINDOW_STALE;
 if(e->closing)return PW_D3D9_WINDOW_CLOSED;
 if(e->failed)return PW_D3D9_WINDOW_BACKEND;
 if(e->pending_sequence)return PW_D3D9_WINDOW_BUSY;
 if(e->sequence==UINT64_MAX)return PW_D3D9_WINDOW_EXHAUSTED;
 if(seq!=e->sequence+1 || !valid_state(state))return PW_D3D9_WINDOW_INVALID;
 e->pending=*state;e->pending_sequence=seq;return PW_D3D9_WINDOW_OK;
}
int pw_d3d9_window_ack(struct pw_d3d9_windows *r,struct pw_d3d9_window_id id,uint64_t seq,uint32_t hresult)
{
 struct pw_d3d9_window_entry *e=entry(r,id);
 if(!e || !seq || seq!=e->pending_sequence)return PW_D3D9_WINDOW_STALE;
 e->backend_result=hresult;e->sequence=seq;e->pending_sequence=0;
 if(hresult&UINT32_C(0x80000000)){e->failed=1;return PW_D3D9_WINDOW_BACKEND;}
 e->applied=e->pending;return PW_D3D9_WINDOW_OK;
}
int pw_d3d9_window_acquire(struct pw_d3d9_windows *r,struct pw_d3d9_window_id id,uint64_t service,struct pw_d3d9_window_lease *lease)
{
 struct pw_d3d9_window_entry *e=entry(r,id);unsigned i;
 if(!e)return PW_D3D9_WINDOW_STALE;
 if(!lease || !service || service!=e->service)return PW_D3D9_WINDOW_INVALID;
 if(e->closing)return PW_D3D9_WINDOW_CLOSED;
 if(e->failed)return PW_D3D9_WINDOW_BACKEND;
 if(!e->sequence || e->pending_sequence)return PW_D3D9_WINDOW_BUSY;
 /* A surface is a resource and may precede ShowWindow. Visibility controls
  * input independently; retain size, ownership and lifetime checks here. */
 if(!e->applied.width || !e->applied.height)return PW_D3D9_WINDOW_HIDDEN;
 if(r->plane_owner.id && !equal(r->plane_owner,id))return PW_D3D9_WINDOW_BUSY;
 for(i=0;i<PW_D3D9_WINDOW_LEASES;i++)if(!r->leases[i].live && r->leases[i].generation<UINT32_MAX){
  r->leases[i].generation++;r->leases[i].live=1;r->leases[i].owner=id;
  lease->epoch=r->epoch;lease->id=i+1;lease->generation=r->leases[i].generation;
  r->plane_owner=id;e->leases++;return PW_D3D9_WINDOW_OK;
 }
 return PW_D3D9_WINDOW_EXHAUSTED;
}
int pw_d3d9_window_release(struct pw_d3d9_windows *r,struct pw_d3d9_window_lease lease)
{
 struct pw_d3d9_window_lease_entry *l;struct pw_d3d9_window_entry *e;
 if(!r || lease.epoch!=r->epoch || !lease.id || lease.id>PW_D3D9_WINDOW_LEASES || !lease.generation)return PW_D3D9_WINDOW_STALE;
 l=&r->leases[lease.id-1];if(!l->live || l->generation!=lease.generation)return PW_D3D9_WINDOW_STALE;
 e=entry(r,l->owner);if(!e || !e->leases || !equal(r->plane_owner,l->owner))return PW_D3D9_WINDOW_INVALID;
 l->live=0;if(!--e->leases)memset(&r->plane_owner,0,sizeof(r->plane_owner));return PW_D3D9_WINDOW_OK;
}
int pw_d3d9_window_close(struct pw_d3d9_windows *r,struct pw_d3d9_window_id id)
{
 struct pw_d3d9_window_entry *e=entry(r,id);if(!e)return PW_D3D9_WINDOW_STALE;
 e->closing=1;return PW_D3D9_WINDOW_OK;
}
int pw_d3d9_window_detach(struct pw_d3d9_windows *r,struct pw_d3d9_window_id id)
{
 struct pw_d3d9_window_entry *e=entry(r,id);if(!e)return PW_D3D9_WINDOW_STALE;
 if(!e->closing || e->leases || e->pending_sequence)return PW_D3D9_WINDOW_BUSY;
 e->live=0;e->guest=e->service=0;return PW_D3D9_WINDOW_OK;
}
uint64_t pw_d3d9_window_input(const struct pw_d3d9_windows *r,uint64_t handle)
{
 unsigned i;if(!r || !handle)return 0;
 for(i=0;i<PW_D3D9_WINDOWS;i++){
  const struct pw_d3d9_window_entry *e=&r->windows[i];
  if(e->live && (e->guest==handle || e->service==handle))return
   !e->closing && !e->failed && e->sequence && (e->applied.flags&PW_D3D9_WINDOW_VISIBLE)?e->guest:0;
 }
 return 0;
}
