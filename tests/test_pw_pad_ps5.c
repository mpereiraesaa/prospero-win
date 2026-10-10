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
/* User 42 is in the foreground (handle 7); user 43 signs in as player 2 (handle 8). */
static int pad_open(int32_t u,int32_t t,int32_t i,const void *p)
{assert((u==42||u==43)&&!t&&!i&&!p);open_calls++;return u==42?7:8;}
static int pad_read(int32_t h,PwPadPs5Data *s,int32_t n)
{assert((h==7||h==8)&&n==64);read_calls++;if(read_rc<0)return read_rc;
 memcpy(s,fixture,(size_t)fixture_count*sizeof(*s));return fixture_count;}
static int pad_close(int32_t h){assert(h==7||h==8);close_calls++;return 0;}
static int vibration_calls,vibration_rc;static uint8_t motors_seen[2];
static int pad_set_vibration(int32_t h,const uint8_t m[2])
{assert(h==7);vibration_calls++;memcpy(motors_seen,m,2);return vibration_rc;}
static int32_t signed_in[PW_PAD_PS5_USERS]={42,-1,-1,-1};static int login_rc;
static int login_users(int32_t ids[PW_PAD_PS5_USERS])
{memcpy(ids,signed_in,sizeof(signed_in));return login_rc;}
static const PwPadPs5Ops ops={user_initialize,foreground,terminate,pad_init,pad_open,pad_read,pad_close,
                              pad_set_vibration,login_users};
enum { CREATE=0x1,L1=0x400 };
static const PwPadKeyMap map[]={{L1,'Z',0,0,"left-flipper"}};

int main(void)
{
    PwPadPs5 pad;
    assert(pw_pad_ps5_open(&pad,&ops,map,1)==PW_OK && pad.opened && pad.pad_handle==7);
    assert(pad.left_stick.x==0x80 && pad.right_stick.y==0x80 && !pad.l2 && !pad.r2);
    assert(init_calls==1&&foreground_calls==1&&pad_init_calls==1&&open_calls==1);
    fixture[0]=(PwPadPs5Data){.buttons=L1|CREATE,.connected=1,.timestamp=1000,.connected_count=1};
    fixture_count=1;assert(pw_pad_ps5_read(&pad)==PW_OK);
    assert(pad.connected_samples==1 && pad.core.pressed_edges==(L1|CREATE));
    fixture_count=0;assert(pw_pad_ps5_read(&pad)==PW_OK &&
                           !pad.core.pressed_edges && !pad.core.released_edges);
    fixture_count=1;fixture[0].buttons=0x80000000u|L1;fixture[0].timestamp=2000;
    assert(pw_pad_ps5_read(&pad)==PW_OK);
    assert(pad.intercepted_samples==1 && pad.core.released_edges==(L1|CREATE) &&
           !pad.core.previous_buttons);
    read_rc=-9;assert(pw_pad_ps5_read(&pad)==PW_OK&&pad.read_errors==1);
    read_rc=0;
    fixture[0]=(PwPadPs5Data){.buttons=CREATE,.connected=1,.timestamp=3000,.connected_count=1};
    fixture_count=1;assert(pw_pad_ps5_read(&pad)==PW_OK);
    assert(pad.core.pressed_edges==CREATE && pad.core.previous_buttons==CREATE);
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
    /* Vibration: both motors in order, a failure kept, nothing without the op. */
    assert(pw_pad_ps5_vibrate(&pad,200,10)==PW_OK && vibration_calls==1 &&
           motors_seen[0]==200 && motors_seen[1]==10 && !pad.vibration_rc);
    vibration_rc=-5;assert(pw_pad_ps5_vibrate(&pad,0,0)==PW_ERR_STATE && pad.vibration_rc==-5);
    vibration_rc=0;
    pad.ops.pad_set_vibration=NULL;
    assert(pw_pad_ps5_vibrate(&pad,1,1)==PW_ERR_UNSUPPORTED && vibration_calls==2);
    pad.ops.pad_set_vibration=pad_set_vibration;
    assert(pw_pad_ps5_vibrate(NULL,1,1)==PW_ERR_PRECONDITION);
    assert(pw_pad_ps5_close(&pad)==PW_OK);
    assert(pw_pad_ps5_vibrate(&pad,1,1)==PW_ERR_PRECONDITION && vibration_calls==2);
    assert(close_calls==1&&terminate_calls==1&&!pad.opened&&!pad.owns_user_service);

    user_init_rc=1;read_rc=0;fixture_count=0;
    assert(pw_pad_ps5_open(&pad,&ops,map,1)==PW_OK&&!pad.owns_user_service);
    assert(pw_pad_ps5_close(&pad)==PW_OK && terminate_calls==1);
    assert(pw_pad_ps5_close(NULL)==PW_ERR_PRECONDITION);
    assert(pw_pad_ps5_read(&pad)==PW_ERR_PRECONDITION);

    /* A second signed-in user's pad for player 2. Nobody else signed in:
     * not found; then user 43 is; its pad reads on its own handle, and
     * closing it leaves the user service to the first pad. */
    PwPadPs5 first,second;
    user_init_rc=0;int terminated=terminate_calls,opened=open_calls;
    assert(pw_pad_ps5_open(&first,&ops,map,1)==PW_OK && first.owns_user_service);
    assert(pw_pad_ps5_open_other(&second,&first,map,1)==PW_ERR_NOT_FOUND && !second.opened);
    signed_in[2]=43;
    assert(pw_pad_ps5_open_other(&second,&first,map,1)==PW_OK);
    assert(second.opened && second.user_id==43 && second.pad_handle==8 && !second.owns_user_service);
    assert(open_calls==opened+2);
    fixture[0]=(PwPadPs5Data){.buttons=L1,.connected=1,.timestamp=4000,.connected_count=1};
    fixture_count=1;assert(pw_pad_ps5_read(&second)==PW_OK && second.core.pressed_edges==L1);
    assert(pw_pad_ps5_close(&second)==PW_OK && terminate_calls==terminated);
    login_rc=-1;assert(pw_pad_ps5_open_other(&second,&first,map,1)==PW_ERR_STATE);login_rc=0;
    first.ops.login_users=NULL;
    assert(pw_pad_ps5_open_other(&second,&first,map,1)==PW_ERR_UNSUPPORTED);
    first.ops.login_users=login_users;
    assert(pw_pad_ps5_close(&first)==PW_OK && terminate_calls==terminated+1);
    assert(pw_pad_ps5_open_other(&second,&first,map,1)==PW_ERR_PRECONDITION);
    return 0;
}
