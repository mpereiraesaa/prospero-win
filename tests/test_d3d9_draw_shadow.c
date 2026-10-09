/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_draw_shadow.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define BAD 0x8876086cu
int main(void)
{
    struct pw_d3d9_draw_shadow s, before;
    struct pw_d3d9_draw_block b = {0}, pixel = {0};
    struct pw_d3d9_command c = {.method=81,.args={4,0,0}};
    pw_d3d9_draw_init(&s);
    assert(!pw_d3d9_draw_can_queue(&s,&c)); /* zero draw still requires declaration */
    pw_d3d9_draw_fvf(&s,0,0); assert(s.active==PW_D3D9_DECL_NULL);
    pw_d3d9_draw_fvf(&s,2,BAD); assert(s.active==PW_D3D9_DECL_NULL);
    pw_d3d9_draw_fvf(&s,2,0); assert(pw_d3d9_draw_can_queue(&s,&c));
    before=s; pw_d3d9_draw_declaration(&s,0,1); assert(!memcmp(&s,&before,sizeof(s)));
    pw_d3d9_draw_begin(&s,0);
    pw_d3d9_draw_declaration(&s,0,0);
    assert(s.active==PW_D3D9_DECL_PRESENT && pw_d3d9_draw_can_queue(&s,&c));
    before=s; pw_d3d9_draw_begin(&s,BAD); assert(!memcmp(&s,&before,sizeof(s)));
    pw_d3d9_draw_end(&s,&b,BAD); assert(s.recording);
    pw_d3d9_draw_end(&s,&b,0); assert(!s.recording && b.captures_decl && b.declaration==PW_D3D9_DECL_NULL);
    pw_d3d9_draw_apply(&s,&b,0); assert(s.active==PW_D3D9_DECL_PRESENT);
    pw_d3d9_draw_declaration(&s,0,0);
    pw_d3d9_draw_begin(&s,0); pw_d3d9_draw_fvf(&s,2,0);
    assert(!pw_d3d9_draw_can_queue(&s,&c)); /* recorded setter is not live */
    pw_d3d9_draw_end(&s,&b,0); pw_d3d9_draw_apply(&s,&b,0);
    assert(pw_d3d9_draw_can_queue(&s,&c));
    pw_d3d9_draw_create_block(&s,&pixel,2,0); assert(!pixel.captures_decl);
    pw_d3d9_draw_declaration(&s,0,0); pw_d3d9_draw_apply(&s,&pixel,0);
    assert(s.active==PW_D3D9_DECL_NULL);
    pw_d3d9_draw_capture(&s,&b,0); assert(b.declaration==PW_D3D9_DECL_NULL);
    pw_d3d9_draw_fvf(&s,2,0); pw_d3d9_draw_apply(&s,&b,0);
    assert(s.active==PW_D3D9_DECL_PRESENT);
    pw_d3d9_draw_create_block(&s,&b,3,0); assert(b.captures_decl);
    pw_d3d9_draw_invalidate(&s); assert(!pw_d3d9_draw_can_queue(&s,&c));
    pw_d3d9_draw_capture(&s,&b,0); assert(b.declaration==PW_D3D9_DECL_UNKNOWN);
    pw_d3d9_draw_observe(&s,1,0); pw_d3d9_draw_apply(&s,&b,0);
    assert(!pw_d3d9_draw_can_queue(&s,&c));
    pw_d3d9_draw_observe(&s,1,0); pw_d3d9_draw_apply(&s,NULL,BAD);
    assert(pw_d3d9_draw_can_queue(&s,&c));
    pw_d3d9_draw_apply(&s,NULL,0); assert(!pw_d3d9_draw_can_queue(&s,&c));
    pw_d3d9_draw_observe(&s,1,0);
    for (unsigned method=81;method<=82;method++) {
        c=(struct pw_d3d9_command){.method=method};
        for(unsigned type=0;type<8;type++) {
            c.args[0]=type;
            assert(pw_d3d9_draw_can_queue(&s,&c)==(type>=1 && type<=6));
        }
        c.args[0]=4; c.args[1]=0xffffffffu; c.args[2]=0xffffffffu;
        assert(pw_d3d9_draw_can_queue(&s,&c));
        c.args[method==81?3:6]=1; assert(!pw_d3d9_draw_can_queue(&s,&c));
        c.args[method==81?3:6]=0; c.data_bytes=4; assert(!pw_d3d9_draw_can_queue(&s,&c));
    }
    c=(struct pw_d3d9_command){.method=57};assert(!pw_d3d9_draw_can_queue(&s,&c));
    assert(!pw_d3d9_draw_can_queue(NULL,&c));assert(!pw_d3d9_draw_can_queue(&s,NULL));
    pw_d3d9_draw_begin(&s,0); pw_d3d9_draw_invalidate(&s); assert(s.recording);
    puts("PASS draw shadow: live/recorded declaration, FVF0, capture/Apply, Reset invalidation, exact draw shape");
    return 0;
}
