/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Reuse the real session/codec peer and tracked allocator from the binding
 * fixture; only this entry point selects pipeline-specific retirement cases. */
#define PW_D3D9_ENABLE_PIPELINE
#define main binding_fixture_original_main
#ifndef PIPELINE_BINDING_SOURCE
#define PIPELINE_BINDING_SOURCE "d3d9_binding_client.c"
#endif
#include PIPELINE_BINDING_SOURCE
#undef main
int main(void)
{
 assert(InitOnceExecuteOnce(&tls_once,init_tls,NULL,NULL));
 SetEnvironmentVariableA("PW_D3D9_ASYNC","1");
 for(unsigned cancel=0;cancel<2;cancel++){
  struct fixture *heap=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*heap));
  assert(heap);mapped_wire=1;begin(heap,0);
  heap->session.broker=heap->thread;watched_free=heap;free_count=0;
  struct ticket_context c={.f=heap,.close_on_drop=1,.close_hr=cancel?E_FAIL:S_OK};
  active_context=&c;
  for(unsigned n=0;n<129;n++)assert(binding(&c)==S_OK);
  assert(heap->session.pipeline.count==1&&heap->session.pipeline_blocks==1);
  assert(c.pins==129&&!c.drops);
  if(cancel)pw_d3d9_session_cancel(&heap->session);
  else{
   struct pw_d3d9_command q={.method=3};
   assert(pw_d3d9_session_command(&heap->session,(struct pw_d3d9_object_ref){7,1},&q)==S_OK);
  }
  assert(free_count==1&&c.pins==0&&c.drops==129);
  watched_free=NULL;mapped_wire=0;
 }
 puts("PIPELINE_LIFETIME PASS published_tickets=128 final_parent_close=1 cancelled_join=1 exactly_one_free=1");
 return 0;
}
