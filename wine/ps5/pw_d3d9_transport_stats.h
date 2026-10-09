/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_TRANSPORT_STATS_H
#define PW_D3D9_TRANSPORT_STATS_H
#include <stdint.h>
#include <stddef.h>
#define PW_D3D9_STATS_OPS 34u
/* Local counters only: never wire storage. Saturation makes invalidity sticky. */
struct pw_d3d9_transport_stats {
 uint64_t attempts,published,replies,failures,request_bytes,reply_bytes,rejected_present;
 uint64_t serial_wait_wall_us,guest_wait_wall_us,roundtrip_wall_us,service_dispatch_wall_us;
 uint64_t clock_invalid,saturated;
 uint64_t async_queued,batch_flushes,batch_commands,batch_residence_wall_us;
 uint64_t pipeline_published,pipeline_acked,pipeline_pending_peak,pipeline_wait_wall_us;
 uint64_t opcode[PW_D3D9_STATS_OPS];
};
static inline uint64_t pw_d3d9_stats_sum(uint64_t a,uint64_t b,uint64_t *invalid)
{if(b>UINT64_MAX-a){*invalid=1;return UINT64_MAX;}return a+b;}
static inline void pw_d3d9_stats_add(struct pw_d3d9_transport_stats *a,const struct pw_d3d9_transport_stats *b)
{
#define ADD(f) a->f=pw_d3d9_stats_sum(a->f,b->f,&a->saturated)
 ADD(pipeline_published);ADD(pipeline_acked);ADD(pipeline_wait_wall_us);
 if(b->pipeline_pending_peak>a->pipeline_pending_peak)a->pipeline_pending_peak=b->pipeline_pending_peak;
 ADD(async_queued);ADD(batch_flushes);ADD(batch_commands);ADD(batch_residence_wall_us);
 ADD(rejected_present);ADD(attempts);ADD(published);ADD(replies);ADD(failures);ADD(request_bytes);ADD(reply_bytes);
 ADD(serial_wait_wall_us);ADD(guest_wait_wall_us);ADD(roundtrip_wall_us);ADD(service_dispatch_wall_us);ADD(clock_invalid);
 for(size_t n=0;n<PW_D3D9_STATS_OPS;n++)a->opcode[n]=pw_d3d9_stats_sum(a->opcode[n],b->opcode[n],&a->saturated);
 a->saturated|=b->saturated;
#undef ADD
}
static inline uint64_t pw_d3d9_stats_ticks_us(uint64_t ticks,uint64_t frequency)
{
 if(!frequency||frequency>UINT64_MAX/1000000u)return 0;
 uint64_t seconds=ticks/frequency;
 if(seconds>UINT64_MAX/1000000u)return 0;
 uint64_t whole=seconds*1000000u,fraction=(ticks%frequency)*1000000u/frequency;
 return fraction>UINT64_MAX-whole?0:whole+fraction;
}
static inline uint64_t pw_d3d9_stats_elapsed(uint64_t begin,uint64_t end,uint64_t *invalid)
{if(!begin||!end||end<begin){(*invalid)++;return 0;}return end-begin;}
#endif
