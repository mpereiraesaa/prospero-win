/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_pad.h"
#include <assert.h>

enum { CREATE=0x1,L1=0x400,R1=0x800,CROSS=0x4000,INTERCEPTED=0x80000000u };
static const PwPadKeyMap map[]={
    {L1,'Z',0,0,"left-flipper"},{R1,0xbf,0,0,"right-flipper"},
    {CROSS,0x20,0,0,"plunger"}
};
int main(void)
{
    PwPad ui;
    assert(pw_pad_init(&ui,NULL,3)==PW_ERR_PRECONDITION);
    assert(pw_pad_init(&ui,map,0)==PW_ERR_PRECONDITION);
    const PwPadKeyMap twice[]={{L1,'Z',0,0,"a"},{L1,'X',0,0,"b"}};
    assert(pw_pad_init(&ui,twice,2)==PW_ERR_PRECONDITION);
    const PwPadKeyMap unnamed[]={{L1,'Z',0,0,""}};
    assert(pw_pad_init(&ui,unnamed,1)==PW_ERR_PRECONDITION);
    assert(pw_pad_init(&ui,map,3)==PW_OK);
    PwPadSample press[]={{.buttons=CROSS,.connected=1,.generation=1,.timestamp_us=1},
                         {.buttons=CROSS|CREATE,.connected=1,.generation=1,.timestamp_us=2}};
    assert(pw_pad_track(&ui,press,2)==PW_OK);
    assert(ui.pressed_edges==(CROSS|CREATE) && !ui.released_edges &&
           ui.previous_buttons==(CROSS|CREATE) && ui.connected);
    assert(ui.stats.batches==1 && ui.stats.samples==2 && ui.stats.max_batch==2);
    assert(pw_pad_track(&ui,NULL,0)==PW_OK && !ui.pressed_edges && !ui.released_edges);
    /* Releasing Create keeps Cross held without a new press. */
    PwPadSample held={.buttons=CROSS,.connected=1,.generation=1,.timestamp_us=3};
    assert(pw_pad_track(&ui,&held,1)==PW_OK && ui.released_edges==CREATE && !ui.pressed_edges);
    /* Interception releases everything. */
    PwPadSample intercepted={.buttons=CROSS|INTERCEPTED,.connected=1,.intercepted=1,
                             .generation=1,.timestamp_us=4};
    assert(pw_pad_track(&ui,&intercepted,1)==PW_OK && !ui.previous_buttons &&
           ui.released_edges==CROSS && !ui.connected);
    /* Disconnection and a new generation release everything. */
    assert(pw_pad_track(&ui,press,1)==PW_OK && ui.previous_buttons==CROSS);
    PwPadSample gone={.buttons=CROSS,.connected=0,.generation=1,.timestamp_us=5};
    assert(pw_pad_track(&ui,&gone,1)==PW_OK && !ui.previous_buttons &&
           ui.released_edges==CROSS && !ui.connected);
    PwPadSample again={.buttons=L1,.connected=1,.generation=1,.timestamp_us=6};
    PwPadSample regen={.buttons=L1,.connected=1,.generation=2,.timestamp_us=7};
    assert(pw_pad_track(&ui,&again,1)==PW_OK && ui.pressed_edges==L1);
    assert(pw_pad_track(&ui,&regen,1)==PW_OK && ui.previous_buttons==L1 &&
           ui.pressed_edges==L1 && ui.released_edges==L1 && ui.generation==2);
    assert(pw_pad_track(NULL,NULL,0)==PW_ERR_PRECONDITION);
    assert(pw_pad_track(&ui,NULL,1)==PW_ERR_PRECONDITION);
    return 0;
}
