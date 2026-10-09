/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_transport_stats.h"
#include <assert.h>
#include <stdio.h>
int main(void){
 struct pw_d3d9_transport_stats total={0},sample={0};
 sample.attempts=sample.published=sample.replies=1;sample.opcode[21]=1;
 sample.request_bytes=80;sample.reply_bytes=80;
 sample.guest_wait_wall_us=pw_d3d9_stats_elapsed(10,40,&sample.clock_invalid);
 pw_d3d9_stats_add(&total,&sample);pw_d3d9_stats_add(&total,&sample);
 assert(total.attempts==2&&total.opcode[21]==2&&total.guest_wait_wall_us==60&&!total.clock_invalid);
 assert(!pw_d3d9_stats_elapsed(40,10,&total.clock_invalid));
 assert(!pw_d3d9_stats_elapsed(0,10,&total.clock_invalid));
 assert(!pw_d3d9_stats_elapsed(10,0,&total.clock_invalid)&&total.clock_invalid==3);
 assert(!pw_d3d9_stats_elapsed(10,10,&total.clock_invalid)&&total.clock_invalid==3);
 total.published=UINT64_MAX;pw_d3d9_stats_add(&total,&sample);assert(total.published==UINT64_MAX&&total.saturated);
 assert(pw_d3d9_stats_ticks_us(3,2)==1500000);
 assert(!pw_d3d9_stats_ticks_us(3,0));
 assert(!pw_d3d9_stats_ticks_us((UINT64_MAX/1000000u)*3+2,3));
 assert(pw_d3d9_stats_ticks_us(UINT64_MAX,1000000)==UINT64_MAX);
 struct pw_d3d9_transport_stats pipeline={0},part={0};
 assert(!pipeline.pipeline_published&&!pipeline.pipeline_acked&&!pipeline.pipeline_pending_peak&&!pipeline.pipeline_wait_wall_us);
 part.pipeline_published=3;part.pipeline_acked=2;part.pipeline_pending_peak=3;part.pipeline_wait_wall_us=17;
 pw_d3d9_stats_add(&pipeline,&part);part.pipeline_pending_peak=2;pw_d3d9_stats_add(&pipeline,&part);
 assert(pipeline.pipeline_published==6&&pipeline.pipeline_acked==4&&pipeline.pipeline_pending_peak==3&&pipeline.pipeline_wait_wall_us==34);
 part.pipeline_pending_peak=8;part.pipeline_published=UINT64_MAX;pw_d3d9_stats_add(&pipeline,&part);
 assert(pipeline.pipeline_published==UINT64_MAX&&pipeline.saturated&&pipeline.pipeline_pending_peak==8);
 struct pw_d3d9_transport_stats empty={0};pw_d3d9_stats_add(&pipeline,&empty);assert(pipeline.pipeline_pending_peak==8);
 puts("transport stats PASS");return 0;
}
