/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_binding_plan.h"
#include "pw_d3d9_command_policy.h"
#include "d3d9/pw_d3d9_kinds.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static const struct {uint32_t method,kind,word;} cases[]={
 {65,PW_D3D9_KIND_TEXTURE_2D,1},{87,PW_D3D9_KIND_VERTEX_DECLARATION,0},
 {92,PW_D3D9_KIND_VERTEX_SHADER,0},{100,PW_D3D9_KIND_VERTEX_BUFFER,1},
 {104,PW_D3D9_KIND_INDEX_BUFFER,0},{107,PW_D3D9_KIND_PIXEL_SHADER,0}
};
static void failure(const struct pw_d3d9_command *c,int status)
{
 struct pw_d3d9_binding b,before;memset(&b,0xac,sizeof(b));before=b;
 assert(pw_d3d9_binding_plan(c,&b)==status&&!memcmp(&b,&before,sizeof(b)));
}
int main(void)
{
 struct pw_d3d9_binding b;failure(NULL,PW_D3D9_BINDING_INVALID);
 for(unsigned n=0;n<sizeof(cases)/sizeof(*cases);n++){
  struct pw_d3d9_command q={.method=cases[n].method},decoded;unsigned char wire[PW_D3D9_COMMAND_MAX];size_t bytes;
  assert(pw_d3d9_binding_plan(&q,NULL)==PW_D3D9_BINDING_INVALID);
  for(unsigned nonnull=0;nonnull<2;nonnull++){
   q.args[cases[n].word]=nonnull?UINT32_MAX:0;q.args[cases[n].word+1]=nonnull?17:0;
   assert(!pw_d3d9_command_encode(wire,sizeof(wire),&bytes,&q));assert(!pw_d3d9_command_decode(&decoded,wire,bytes));
   assert(pw_d3d9_binding_plan(&decoded,&b)==PW_D3D9_BINDING_READY);
   assert(b.method==q.method&&b.kind==cases[n].kind&&b.object_word==cases[n].word&&b.id==q.args[b.object_word]&&b.generation==q.args[b.object_word+1]);
   assert(!pw_d3d9_command_can_queue(&q)); /* No accidental activation. */
  }
  q.args[cases[n].word]=0;failure(&q,PW_D3D9_BINDING_INVALID);
  q.args[cases[n].word]=1;q.args[cases[n].word+1]=0;failure(&q,PW_D3D9_BINDING_INVALID);
  q.args[cases[n].word+1]=1;q.args[7]=1;failure(&q,PW_D3D9_BINDING_INVALID);
  q.args[7]=0;q.data_bytes=4;failure(&q,PW_D3D9_BINDING_INVALID);
 }
 const uint32_t samplers[]={0,15,16,255,256,260,261,UINT32_MAX};
 for(unsigned i=0;i<sizeof(samplers)/sizeof(*samplers);i++){
  struct pw_d3d9_command q={.method=65,.args={samplers[i],1,2}};
  int valid=samplers[i]<16||(samplers[i]>=256&&samplers[i]<=260);
  if(valid)assert(pw_d3d9_binding_plan(&q,&b)==PW_D3D9_BINDING_READY);
  else failure(&q,PW_D3D9_BINDING_SYNCHRONOUS);
 }
 struct pw_d3d9_command stream={.method=100,.args={15,1,2,UINT32_MAX,UINT32_MAX}};
 assert(pw_d3d9_binding_plan(&stream,&b)==PW_D3D9_BINDING_READY);
 stream.args[0]=16;failure(&stream,PW_D3D9_BINDING_INVALID);
 stream.args[0]=UINT32_MAX;failure(&stream,PW_D3D9_BINDING_INVALID);
 for(unsigned method=0;method<120;method++){
  int known=0;for(unsigned n=0;n<sizeof(cases)/sizeof(*cases);n++)known|=method==cases[n].method;
  if(!known){struct pw_d3d9_command q={.method=method};failure(&q,PW_D3D9_BINDING_OTHER);}
 }
 puts("BINDING_PLAN PASS six_methods=1 canonical=1 null=1 bounds=1 no_activation=1");return 0;
}
