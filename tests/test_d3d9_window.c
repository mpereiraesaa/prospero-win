/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "wine/ps5/pw_d3d9_window.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#define OK(x) assert((x)==PW_D3D9_WINDOW_OK)
int main(void)
{
 struct pw_d3d9_windows r;struct pw_d3d9_window_id a,b,old,bad;
 struct pw_d3d9_window_lease first,second,stale,full[PW_D3D9_WINDOW_LEASES];
 struct pw_d3d9_window_state state={20,30,320,240,PW_D3D9_WINDOW_VISIBLE|PW_D3D9_WINDOW_FOCUSED};
 struct pw_d3d9_window_entry view;uint64_t guest=0x1234,service=UINT64_C(0x100001234);
 assert(pw_d3d9_windows_init(&r,0)==PW_D3D9_WINDOW_INVALID);OK(pw_d3d9_windows_init(&r,7));
 OK(pw_d3d9_window_attach(&r,guest,service,&a));old=a;
 assert(a.epoch==7 && a.id==1 && a.generation==1);
 assert(pw_d3d9_window_attach(&r,service,99,&b)==PW_D3D9_WINDOW_BUSY);
 assert(pw_d3d9_window_attach(&r,1,guest,&b)==PW_D3D9_WINDOW_BUSY);
 assert(pw_d3d9_window_attach(&r,1,1,&b)==PW_D3D9_WINDOW_INVALID);
 OK(pw_d3d9_window_attach(&r,0x2345,UINT64_C(0x200002345),&b));
 bad=a;bad.epoch++;assert(pw_d3d9_window_get(&r,bad,&view)==PW_D3D9_WINDOW_STALE);
 assert(!pw_d3d9_window_input(&r,service));
 assert(pw_d3d9_window_acquire(&r,a,service,&first)==PW_D3D9_WINDOW_BUSY);
 assert(pw_d3d9_window_begin(&r,a,2,&state)==PW_D3D9_WINDOW_INVALID);
 OK(pw_d3d9_window_begin(&r,a,1,&state));state.width=999;
 assert(pw_d3d9_window_begin(&r,a,1,&state)==PW_D3D9_WINDOW_BUSY);
 assert(pw_d3d9_window_ack(&r,a,2,0)==PW_D3D9_WINDOW_STALE);
 OK(pw_d3d9_window_ack(&r,a,1,1)); /* S_FALSE is not a failed HRESULT. */
 OK(pw_d3d9_window_get(&r,a,&view));assert(view.applied.width==320 && view.backend_result==1);
 assert(pw_d3d9_window_input(&r,service)==guest && pw_d3d9_window_input(&r,guest)==guest);
 assert(!pw_d3d9_window_input(&r,999));
 assert(pw_d3d9_window_find(&r,999,&bad)==PW_D3D9_WINDOW_STALE);
 OK(pw_d3d9_window_find(&r,service,&bad));assert(bad.id==a.id && bad.generation==a.generation);
 assert(pw_d3d9_window_acquire(&r,a,guest,&first)==PW_D3D9_WINDOW_INVALID);
 OK(pw_d3d9_window_acquire(&r,a,service,&first));stale=first;
 OK(pw_d3d9_window_acquire(&r,a,service,&second)); /* Reset overlap, same pair. */
 OK(pw_d3d9_window_begin(&r,b,1,&state));OK(pw_d3d9_window_ack(&r,b,1,0));
 assert(pw_d3d9_window_acquire(&r,b,UINT64_C(0x200002345),&full[0])==PW_D3D9_WINDOW_BUSY);
 OK(pw_d3d9_window_close(&r,a));assert(!pw_d3d9_window_input(&r,service));
 OK(pw_d3d9_window_find(&r,service,&bad)); /* Known pair with suppressed input, not an ordinary HWND. */
 assert(pw_d3d9_window_acquire(&r,a,service,&full[0])==PW_D3D9_WINDOW_CLOSED);
 assert(pw_d3d9_window_detach(&r,a)==PW_D3D9_WINDOW_BUSY);
 OK(pw_d3d9_window_release(&r,first));assert(pw_d3d9_window_release(&r,first)==PW_D3D9_WINDOW_STALE);
 assert(r.plane_owner.id==a.id);OK(pw_d3d9_window_release(&r,second));assert(!r.plane_owner.id);
 OK(pw_d3d9_window_detach(&r,a));OK(pw_d3d9_window_attach(&r,guest,service,&a));
 assert(a.id==old.id && a.generation==old.generation+1);
 assert(pw_d3d9_window_get(&r,old,&view)==PW_D3D9_WINDOW_STALE);
 state.flags=0;OK(pw_d3d9_window_begin(&r,a,1,&state));OK(pw_d3d9_window_ack(&r,a,1,0));
 OK(pw_d3d9_window_acquire(&r,a,service,&first));
 assert(!pw_d3d9_window_input(&r,service));
 OK(pw_d3d9_window_get(&r,a,&view));assert(!view.applied.flags && view.leases==1);
 assert(pw_d3d9_window_acquire(&r,b,UINT64_C(0x200002345),&second)==PW_D3D9_WINDOW_BUSY);
 OK(pw_d3d9_window_release(&r,first));assert(!r.plane_owner.id);
 state.flags=PW_D3D9_WINDOW_VISIBLE;OK(pw_d3d9_window_begin(&r,a,2,&state));
 assert(pw_d3d9_window_ack(&r,a,2,UINT32_C(0x88760868))==PW_D3D9_WINDOW_BACKEND);
 OK(pw_d3d9_window_get(&r,a,&view));assert(view.backend_result==UINT32_C(0x88760868));
 assert(pw_d3d9_window_begin(&r,a,3,&state)==PW_D3D9_WINDOW_BACKEND);
 assert(pw_d3d9_window_acquire(&r,a,service,&first)==PW_D3D9_WINDOW_BACKEND);
 OK(pw_d3d9_window_close(&r,a));OK(pw_d3d9_window_detach(&r,a));
 /* Every live surface must have a distinct lease, including reset overlap. */
 for(unsigned i=0;i<PW_D3D9_WINDOW_LEASES;i++)OK(pw_d3d9_window_acquire(&r,b,UINT64_C(0x200002345),&full[i]));
 assert(pw_d3d9_window_acquire(&r,b,UINT64_C(0x200002345),&first)==PW_D3D9_WINDOW_EXHAUSTED);
 assert(pw_d3d9_window_release(&r,stale)==PW_D3D9_WINDOW_STALE);
 for(unsigned i=0;i<PW_D3D9_WINDOW_LEASES;i++)OK(pw_d3d9_window_release(&r,full[i]));
 /* A close cannot discard an outstanding mirror completion. */
 OK(pw_d3d9_window_begin(&r,b,2,&state));OK(pw_d3d9_window_close(&r,b));
 assert(pw_d3d9_window_detach(&r,b)==PW_D3D9_WINDOW_BUSY);
 OK(pw_d3d9_window_ack(&r,b,2,0));OK(pw_d3d9_window_detach(&r,b));
 OK(pw_d3d9_windows_init(&r,8));assert(pw_d3d9_window_release(&r,full[0])==PW_D3D9_WINDOW_STALE);
 for(unsigned i=0;i<PW_D3D9_WINDOWS;i++)r.windows[i].generation=UINT32_MAX;
 assert(pw_d3d9_window_attach(&r,1,2,&a)==PW_D3D9_WINDOW_EXHAUSTED);
 r.windows[0].generation=0;OK(pw_d3d9_window_attach(&r,1,2,&a));
 state.x=INT32_MAX;assert(pw_d3d9_window_begin(&r,a,1,&state)==PW_D3D9_WINDOW_INVALID);
 state.x=0;state.flags=8;assert(pw_d3d9_window_begin(&r,a,1,&state)==PW_D3D9_WINDOW_INVALID);
 r.windows[0].sequence=UINT64_MAX;assert(pw_d3d9_window_begin(&r,a,0,&state)==PW_D3D9_WINDOW_EXHAUSTED);
 puts("PASS D3D9 window identities, owned mirrors, input owner, display leases, failure and teardown");return 0;
}
