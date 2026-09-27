/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../native/pw_pad_ps5.h"
#include <assert.h>
#include <string.h>

static PwPadPs5Data fixture[64];static int fixture_count,read_rc,user_init_rc;
static int init_calls,foreground_calls,terminate_calls,pad_init_calls,open_calls,read_calls,close_calls;
static int user_initialize(const void *p){assert(!p);init_calls++;return user_init_rc;}
static int foreground(int32_t *u){foreground_calls++;*u=42;return 0;}
static int terminate(void){terminate_calls++;return 0;}
static int pad_init(void){pad_init_calls++;return 0;}
static int pad_open(int32_t u,int32_t t,int32_t i,const void *p)
{assert(u==42&&!t&&!i&&!p);open_calls++;return 7;}
static int pad_read(int32_t h,PwPadPs5Data *s,int32_t n)
{assert(h==7&&n==64);read_calls++;if(read_rc<0)return read_rc;
 memcpy(s,fixture,(size_t)fixture_count*sizeof(*s));return fixture_count;}
static int pad_close(int32_t h){assert(h==7);close_calls++;return 0;}
static const PwPadPs5Ops ops={user_initialize,foreground,terminate,pad_init,pad_open,pad_read,pad_close};
enum { CREATE=0x1,L1=0x400 };
static const PwPadKeyMap map[]={{L1,'Z',0,0,"left-flipper"}};

int main(void)
{
    PwUser32 user;PwUser32Message messages[1];PwUser32Window windows[1];PwPadPs5 pad;
    assert(pw_user32_init(&user,messages,1,windows,1)==PW_OK);
    windows[0]=(PwUser32Window){.handle=0x10000,.wndproc=1,.module=1,.used=1};
    assert(pw_pad_ps5_open(&pad,&ops,map,1)==PW_OK && pad.opened && pad.pad_handle==7);
    assert(pad.left_stick.x==0x80 && pad.right_stick.y==0x80 && !pad.l2 && !pad.r2);
    assert(init_calls==1&&foreground_calls==1&&pad_init_calls==1&&open_calls==1);
    fixture[0]=(PwPadPs5Data){.buttons=L1|CREATE,.connected=1,.timestamp=1000,.connected_count=1};
    fixture_count=1;assert(pw_pad_ps5_poll(&pad,&user,0x10000)==PW_OK);
    assert(user.queue_count==1&&user.queue[0].message==0x100&&pad.connected_samples==1 &&
           (pad.core.pressed_edges&CREATE));
    fixture_count=0;assert(pw_pad_ps5_poll(&pad,&user,0x10000)==PW_OK &&
                           !pad.core.pressed_edges && !pad.core.released_edges);
    fixture_count=1;fixture[0].buttons=0x80000000u|L1;fixture[0].timestamp=2000;
    assert(pw_pad_ps5_poll(&pad,&user,0x10000)==PW_OK);
    assert(user.queue_count==2&&user.queue[1].message==0x101&&pad.intercepted_samples==1);
    read_rc=-9;assert(pw_pad_ps5_poll(&pad,&user,0x10000)==PW_OK&&pad.read_errors==1);
    /* The native-UI read reports edges without posting Win32 keys. */
    read_rc=0;unsigned queued=user.queue_count;
    fixture[0]=(PwPadPs5Data){.buttons=CREATE,.connected=1,.timestamp=3000,.connected_count=1};
    fixture_count=1;assert(pw_pad_ps5_read(&pad)==PW_OK);
    assert(pad.core.pressed_edges==CREATE && pad.core.previous_buttons==CREATE &&
           user.queue_count==queued);
    /* Sticks and triggers follow the newest connected sample... */
    fixture[0]=(PwPadPs5Data){.buttons=CREATE,.left_stick={10,250},.right_stick={200,40},
                              .l2=30,.r2=255,.connected=1,.timestamp=3100,.connected_count=1};
    fixture_count=1;assert(pw_pad_ps5_read(&pad)==PW_OK);
    assert(pad.left_stick.x==10 && pad.left_stick.y==250 && pad.right_stick.x==200 &&
           pad.right_stick.y==40 && pad.l2==30 && pad.r2==255);
    /* ... an empty read keeps them, an intercepted or disconnected one centres them. */
    fixture_count=0;assert(pw_pad_ps5_read(&pad)==PW_OK && pad.left_stick.x==10);
    fixture[0].buttons=0x80000000u|CREATE;fixture_count=1;
    assert(pw_pad_ps5_read(&pad)==PW_OK && pad.left_stick.x==0x80 && !pad.r2);
    fixture[0]=(PwPadPs5Data){.left_stick={1,1},.connected=1,.timestamp=3200,.connected_count=1};
    assert(pw_pad_ps5_read(&pad)==PW_OK && pad.left_stick.x==1);
    fixture[0].connected=0;assert(pw_pad_ps5_read(&pad)==PW_OK && pad.left_stick.x==0x80);
    fixture[0]=(PwPadPs5Data){.buttons=CREATE,.right_stick={9,9},.connected=1,.timestamp=3300,
                              .connected_count=1};
    assert(pw_pad_ps5_read(&pad)==PW_OK && pad.right_stick.x==9);
    fixture_count=0;assert(pw_pad_ps5_read(&pad)==PW_OK && !pad.core.pressed_edges);
    read_rc=-9;assert(pw_pad_ps5_read(&pad)==PW_OK && pad.read_errors==2 &&
                      pad.core.released_edges==CREATE && !pad.core.previous_buttons &&
                      pad.right_stick.x==0x80);
    read_rc=0;
    assert(pw_pad_ps5_close(&pad,&user,0x10000)==PW_OK);
    assert(close_calls==1&&terminate_calls==1&&!pad.opened&&!pad.owns_user_service);

    user_init_rc=1;read_rc=0;fixture_count=0;
    assert(pw_pad_ps5_open(&pad,&ops,map,1)==PW_OK&&!pad.owns_user_service);
    assert(pw_pad_ps5_close(&pad,&user,0x10000)==PW_OK && terminate_calls==1);
    assert(pw_pad_ps5_read(&pad)==PW_ERR_PRECONDITION);
    return 0;
}
