/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define SONAME_LIBVULKAN 1
#define TRUE 1
#define FALSE 0
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
typedef void *HWND;
typedef uintptr_t UINT_PTR,ULONG_PTR,VkInstance;
typedef uint64_t UINT64,VkSurfaceKHR;
typedef uint32_t UINT,DWORD;
typedef int BOOL,VkResult;
#define VK_SUCCESS 0
static pthread_mutex_t screen_lock=PTHREAD_MUTEX_INITIALIZER;
static struct {int WowTebOffset;} teb;
static uint64_t current_token;
static DWORD current_tid;
static int token_query_fail;
static struct {HWND hwnd;DWORD tid,pid;} actual[8];
#define NtCurrentTeb() (&teb)
#define NtCurrentThread() (-2)
#define GetCurrentThreadId() current_tid
#define GetCurrentProcessId() 10u
static int NtQueryInformationThread(int thread,unsigned cls,void *p,unsigned size,void *returned)
{
 uint64_t *words=p;(void)returned;
 assert(thread==-2 && cls==0x50570002 && size==16);
 if(token_query_fail)return -1;
 words[1]=current_token;return current_token?0:-1;
}
static DWORD get_window_thread(HWND hwnd,DWORD *pid)
{
 unsigned i;for(i=0;i<ARRAY_SIZE(actual);i++)if(actual[i].hwnd==hwnd){*pid=actual[i].pid;return actual[i].tid;}
 return 0;
}
struct client_surface {HWND hwnd;};
static int is_client_surface_window(struct client_surface *c,HWND hwnd)
{
 /* Assert runtime adapter drops its own lock before Wine surface lookup. */
 assert(!pthread_mutex_trylock(&screen_lock));pthread_mutex_unlock(&screen_lock);
 return c->hwnd==hwnd;
}
#include "../wine/ps5/pw_d3d9_window_driver.c"
static void domain(DWORD tid,uint64_t token,int guest){current_tid=tid;current_token=token;teb.WowTebOffset=guest?0x2000:0;}
static void created(unsigned slot,uintptr_t hwnd,DWORD tid,uint64_t token,int guest)
{
 domain(tid,token,guest);actual[slot].hwnd=(HWND)hwnd;actual[slot].tid=tid;actual[slot].pid=10;
 assert(bridge_window_created((HWND)hwnd)==!!token);
}
int main(void)
{
 struct pw_d3d9_window_driver_request q={.version=1,.size=sizeof(q),.operation=PW_D3D9_WINDOW_ATTACH,.guest=100,.service=200};
 struct client_surface client={(HWND)200};struct bridge_surface *s,*replacement;
 struct pw_d3d9_window_id first;uint64_t epoch;
 assert(sizeof(q)==80);
 created(0,100,1,0,1);created(1,200,2,10,0);
 assert(!bridge_surface_reserve(&client)); /* native unregistered probe */
 domain(1,0,1);assert(ps5_bridge_window_call(&q,sizeof(q))==PW_D3D9_WINDOW_INVALID);
 domain(2,10,0);
 assert(ps5_bridge_window_call(NULL,sizeof(q))==PW_D3D9_WINDOW_INVALID);
 assert(ps5_bridge_window_call(&q,sizeof(q)-1)==PW_D3D9_WINDOW_INVALID);
 q.reserved=1;assert(ps5_bridge_window_call(&q,sizeof(q))==PW_D3D9_WINDOW_INVALID);q.reserved=0;
 token_query_fail=1;assert(ps5_bridge_window_call(&q,sizeof(q))==PW_D3D9_WINDOW_INVALID);token_query_fail=0;
 actual[0].pid=99;assert(ps5_bridge_window_call(&q,sizeof(q))==PW_D3D9_WINDOW_INVALID);actual[0].pid=10;
 domain(1,0,1);s=bridge_surface_reserve(&client);assert(s);bridge_surface_finish(s,7,8,VK_SUCCESS);
 domain(2,10,0);assert(ps5_bridge_window_call(&q,sizeof(q))==PW_D3D9_WINDOW_BUSY);
 ps5_bridge_surface_destroyed(7,8);
 assert(!ps5_bridge_window_call(&q,sizeof(q)));first=q.id;epoch=q.id.epoch;
 assert(bridge_service_window((HWND)200));assert(!bridge_input_window((HWND)200));
 assert(bridge_input_window((HWND)333)==(HWND)333);
 q.operation=PW_D3D9_WINDOW_BEGIN;q.sequence=1;q.state=(struct pw_d3d9_window_state){0,0,640,480,1};
 assert(!ps5_bridge_window_call(&q,sizeof(q)));assert(!bridge_surface_reserve(&client));
 q.operation=PW_D3D9_WINDOW_ACK;assert(!ps5_bridge_window_call(&q,sizeof(q)));
 assert(bridge_input_window((HWND)200)==(HWND)100);
 s=bridge_surface_reserve(&client);assert(s);bridge_surface_finish(s,11,22,VK_SUCCESS);
 replacement=bridge_surface_reserve(&client);assert(replacement);bridge_surface_finish(replacement,11,23,-1);
 assert(bridge_windows.windows[0].leases==1);
 domain(3,0,1);assert(!bridge_surface_reserve(&client));domain(2,10,0);
 client.hwnd=(HWND)333;assert(!bridge_surface_reserve(&client));client.hwnd=(HWND)200;
 q.operation=PW_D3D9_WINDOW_CLOSE;assert(!ps5_bridge_window_call(&q,sizeof(q)));
 assert(!bridge_input_window((HWND)200));assert(!bridge_surface_reserve(&client));
 q.operation=PW_D3D9_WINDOW_DETACH;assert(ps5_bridge_window_call(&q,sizeof(q))==PW_D3D9_WINDOW_BUSY);
 pthread_mutex_lock(&screen_lock);bridge_window_destroyed((HWND)100);bridge_window_destroyed((HWND)200);pthread_mutex_unlock(&screen_lock);
 memset(actual,0,sizeof(actual));
 ps5_bridge_surface_destroyed(11,999);assert(bridge_windows.windows[0].leases==1);
 ps5_bridge_surface_destroyed(11,22);ps5_bridge_surface_destroyed(11,22);
 assert(!ps5_bridge_window_call(&q,sizeof(q)));assert(!bridge_token);
 assert(!bridge_owner((HWND)100) && !bridge_owner((HWND)200));
 created(0,100,1,0,1);created(1,200,4,11,0);q.operation=PW_D3D9_WINDOW_ATTACH;
 assert(!ps5_bridge_window_call(&q,sizeof(q)));assert(q.id.epoch>epoch);
 q.id=first;q.operation=PW_D3D9_WINDOW_CLOSE;assert(ps5_bridge_window_call(&q,sizeof(q))==PW_D3D9_WINDOW_STALE);
 /* Exhaustion rejects native creation and suppresses unknown input/geometry. */
 domain(1,0,1);
 for(unsigned i=0;i<ARRAY_SIZE(bridge_owners);i++)if(!bridge_owners[i].hwnd)
  assert(!bridge_window_created((HWND)(uintptr_t)(1000+i)));
 assert(bridge_owner_full());domain(4,11,0);
 assert(bridge_window_created((HWND)9000)==-1);
 assert(!bridge_input_window((HWND)9000) && bridge_service_window((HWND)9000));
 client.hwnd=(HWND)9000;assert(!bridge_surface_reserve(&client));
 puts("PASS driver token/owner binding, mirror, input, surface failure/replacement, teardown and stale epoch");return 0;
}
