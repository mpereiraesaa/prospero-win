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
 puts("transport stats PASS");return 0;
}
