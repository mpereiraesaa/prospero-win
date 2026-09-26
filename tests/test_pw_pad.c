/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_pad.h"
#include <assert.h>
#include <string.h>

enum { CREATE=0x1,L1=0x400,R1=0x800,CROSS=0x4000,INTERCEPTED=0x80000000u };
static const PwPadKeyMap map[]={
    {L1,'Z',0,0,"left-flipper"},{R1,0xbf,0,0,"right-flipper"},
    {CROSS,0x20,0,0,"plunger"}
};
static void setup(PwUser32 *user,PwUser32Message *messages,PwUser32Window *windows)
{
    assert(pw_user32_init(user,messages,1,windows,1)==PW_OK);
    windows[0]=(PwUser32Window){.handle=0x10000,.wndproc=1,.module=1,.used=1};
}
int main(void)
{
    PwUser32 user;PwUser32Message messages[1];PwUser32Window windows[1];PwPad pad;
    setup(&user,messages,windows);assert(pw_pad_init(&pad,map,3)==PW_OK);
    PwPadSample batch[]={
        {.connected=1,.generation=1,.timestamp_us=1000},
        {.buttons=L1|CROSS|CREATE,.connected=1,.generation=1,.timestamp_us=2000},
        {.buttons=L1|CROSS,.connected=1,.generation=1,.timestamp_us=3000},
        {.buttons=R1,.connected=1,.generation=1,.timestamp_us=4000},
    };
    assert(pw_pad_process(&pad,&user,0x10000,batch,4)==PW_OK);
    assert(user.queue_count==5 && user.queue[0].message==0x100 && user.queue[0].wparam=='Z');
    assert(user.queue[0].lparam==(1u|(0x2cu<<16)) && user.queue[0].time==2);
    assert(user.queue[1].wparam==0x20 && user.queue[1].message==0x100);
    assert(user.queue[2].wparam=='Z' && user.queue[2].message==0x101 &&
           user.queue[2].lparam==(1u|(0x2cu<<16)|0xc0000000u));
    assert(user.queue[3].wparam==0xbf && user.queue[3].message==0x100);
    assert(user.queue[4].wparam==0x20 && user.queue[4].message==0x101);
    assert(pad.stats.samples==4 && pad.stats.events==5 && pad.stats.presses==3 &&
           pad.stats.releases==2 && pad.stats.max_batch==4);
    assert((pad.pressed_edges&CREATE) && (pad.released_edges&CREATE));

    user.queue_count=0;
    PwPadSample generation={.buttons=CROSS,.connected=1,.generation=2,.timestamp_us=5000};
    assert(pw_pad_process(&pad,&user,0x10000,&generation,1)==PW_OK);
    assert(user.queue_count==2 && user.queue[0].wparam==0xbf && user.queue[0].message==0x101 &&
           user.queue[1].wparam==0x20 && user.queue[1].message==0x100);
    assert(pad.stats.neutralizations==1);
    user.queue_count=0;
    PwPadSample intercepted={.buttons=CROSS|INTERCEPTED,.connected=1,.intercepted=1,
                             .generation=2,.timestamp_us=6000};
    assert(pw_pad_process(&pad,&user,0x10000,&intercepted,1)==PW_OK);
    assert(user.queue_count==1 && user.queue[0].wparam==0x20 && user.queue[0].message==0x101);
    assert(!pad.previous_buttons && !pad.connected && pad.stats.neutralizations==2);

    user.queue_count=PW_USER32_QUEUE_CAPACITY;
    generation=(PwPadSample){.buttons=L1,.connected=1,.generation=2,.timestamp_us=7000};
    assert(pw_pad_process(&pad,&user,0x10000,&generation,1)==PW_ERR_LIMIT);
    assert(!pad.previous_buttons); /* fail closed; edge can be retried */

    /* A native UI tracks edges without any Win32 destination. */
    PwPad ui;assert(pw_pad_init(&ui,map,3)==PW_OK);
    PwPadSample press[]={{.buttons=CROSS,.connected=1,.generation=1,.timestamp_us=1},
                         {.buttons=CROSS|CREATE,.connected=1,.generation=1,.timestamp_us=2}};
    assert(pw_pad_track(&ui,press,2)==PW_OK);
    assert(ui.pressed_edges==(CROSS|CREATE) && !ui.released_edges &&
           ui.previous_buttons==(CROSS|CREATE) && ui.connected && !ui.stats.events);
    assert(pw_pad_track(&ui,NULL,0)==PW_OK && !ui.pressed_edges && !ui.released_edges);
    /* Handing the same pad to a guest while Cross is still held: the held
     * button is not replayed as a press, and its release is a key-up. */
    user.queue_count=0;
    PwPadSample held={.buttons=CROSS,.connected=1,.generation=1,.timestamp_us=3};
    assert(pw_pad_process(&ui,&user,0x10000,&held,1)==PW_OK);
    assert(user.queue_count==0 && ui.released_edges==CREATE && !ui.pressed_edges);
    PwPadSample none={.connected=1,.generation=1,.timestamp_us=4};
    assert(pw_pad_process(&ui,&user,0x10000,&none,1)==PW_OK);
    assert(user.queue_count==1 && user.queue[0].wparam==0x20 && user.queue[0].message==0x101);
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
