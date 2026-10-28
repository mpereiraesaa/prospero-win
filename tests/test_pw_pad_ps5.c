/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../native/pw_pad_ps5.h"
#include <assert.h>
#include <limits.h>
#include <string.h>

static PwPadPs5Data fixture[64];static int fixture_count,read_rc,user_init_rc;
static int init_calls,foreground_calls,terminate_calls,pad_init_calls,open_calls,read_calls,close_calls;
static int user_initialize(const void *p){assert(!p);init_calls++;return user_init_rc;}
static int foreground(int32_t *u){foreground_calls++;*u=42;return 0;}
static int terminate(void){terminate_calls++;return 0;}
static int pad_init(void){pad_init_calls++;return 0;}
/* User 42 is in the foreground (handle 7); user 43 signs in as player 2 (handle 8). */
static int pad_open(int32_t u,int32_t t,int32_t i,const void *p)
{assert((u==42||u==43||u==44)&&!t&&!i&&!p);open_calls++;return u==42?7:8;}
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
static int vibration_mode_calls,vibration_mode_seen;
static int pad_set_vibration_mode(int32_t h,int32_t m)
{assert(h==7);vibration_mode_calls++;vibration_mode_seen=m;return 0;}
static int trigger_calls,trigger_rc;static PwPadPs5TriggerEffect trigger_seen;
static int pad_set_trigger_effect(int32_t h,const PwPadPs5TriggerEffect *e)
{assert(h==7);trigger_calls++;trigger_seen=*e;return trigger_rc;}
static const PwPadPs5Ops ops={user_initialize,foreground,terminate,pad_init,pad_open,pad_read,pad_close,
                              pad_set_vibration,login_users,pad_set_vibration_mode,
                              pad_set_trigger_effect};
enum { CREATE=0x1,L1=0x400,TOUCHPAD=0x100000 };
static const PwPadKeyMap map[]={{L1,'Z',0,0,"left-flipper"}};

/* L2 and R2 vibrate with the motors: amplitude 0..8 from the stronger one,
 * 30 Hz when the large one leads and 60 Hz when the small one does, set only
 * when that changes. A failed effect leaves the rumble's result alone and
 * is not tried again until the rumble changes. */
static void test_trigger_rumble(void)
{
    PwPadPs5 pad;
    assert(pw_pad_ps5_open(&pad,&ops,map,1)==PW_OK && pad.trigger_rc==INT_MIN);
    int calls=trigger_calls;
    assert(pw_pad_ps5_vibrate(&pad,255,10)==PW_OK && trigger_calls==calls+1 && !pad.trigger_rc);
    assert(trigger_seen.mask==(PW_PAD_PS5_TRIGGER_L2|PW_PAD_PS5_TRIGGER_R2));
    for(int i=0;i<2;i++)
        assert(trigger_seen.command[i].mode==PW_PAD_PS5_TRIGGER_VIBRATION &&
               trigger_seen.command[i].data[0]==1 && trigger_seen.command[i].data[1]==8 &&
               trigger_seen.command[i].data[2]==30);
    assert(pw_pad_ps5_vibrate(&pad,254,0)==PW_OK && trigger_calls==calls+1);   /* unchanged */
    assert(pw_pad_ps5_vibrate(&pad,0,40)==PW_OK && trigger_calls==calls+2 &&
           trigger_seen.command[1].data[1]==2 && trigger_seen.command[1].data[2]==60);
    trigger_rc=-1;
    assert(pw_pad_ps5_vibrate(&pad,0,0)==PW_OK && pad.trigger_rc==-1 && trigger_calls==calls+3);
    assert(pw_pad_ps5_vibrate(&pad,0,0)==PW_OK && trigger_calls==calls+3);   /* not retried */
    trigger_rc=0;
    assert(pw_pad_ps5_vibrate(&pad,100,0)==PW_OK && trigger_calls==calls+4 && !pad.trigger_rc &&
           trigger_seen.command[0].data[1]==4);
    assert(pw_pad_ps5_vibrate(&pad,0,0)==PW_OK && trigger_calls==calls+5 &&
           trigger_seen.command[0].mode==PW_PAD_PS5_TRIGGER_OFF && !trigger_seen.command[0].data[1]);
    assert(pw_pad_ps5_vibrate(&pad,0,0)==PW_OK && trigger_calls==calls+5);
    vibration_rc=-5;   /* a failed rumble is still the call's result */
    assert(pw_pad_ps5_vibrate(&pad,100,0)==PW_ERR_STATE && trigger_calls==calls+6);
    vibration_rc=0;
    assert(pw_pad_ps5_close(&pad)==PW_OK);
    init_calls=foreground_calls=pad_init_calls=open_calls=read_calls=close_calls=terminate_calls=0;
    vibration_calls=vibration_mode_calls=0;
}

int main(void)
{
    test_trigger_rumble();
    PwPadPs5 pad;
    assert(pw_pad_ps5_open(&pad,&ops,map,1)==PW_OK && pad.opened && pad.pad_handle==7);
    assert(pad.left_stick.x==0x80 && pad.right_stick.y==0x80 && !pad.l2 && !pad.r2);
    assert(init_calls==1&&foreground_calls==1&&pad_init_calls==1&&open_calls==1);
    assert(vibration_mode_calls==1 && vibration_mode_seen==PW_PAD_PS5_VIBRATION_COMPATIBLE);
    fixture[0]=(PwPadPs5Data){.buttons=L1|CREATE,.connected=1,.timestamp=1000,.connected_count=1};
    fixture_count=1;assert(pw_pad_ps5_read(&pad)==PW_OK);
    assert(pad.connected_samples==1 && pad.core.pressed_edges==(L1|CREATE));
    fixture_count=0;assert(pw_pad_ps5_read(&pad)==PW_OK &&
                           !pad.core.pressed_edges && !pad.core.released_edges);
    fixture_count=1;fixture[0].buttons=0x80000000u|L1;fixture[0].timestamp=2000;
    assert(pw_pad_ps5_read(&pad)==PW_OK);
    assert(pad.intercepted && !pad.core.connected);
    assert(pad.intercepted_samples==1 && pad.core.released_edges==(L1|CREATE) &&
           !pad.core.previous_buttons);
    read_rc=-9;assert(pw_pad_ps5_read(&pad)==PW_OK&&pad.read_errors==1&&!pad.intercepted);
    read_rc=0;
    fixture[0]=(PwPadPs5Data){.buttons=CREATE,.connected=1,.timestamp=3000,.connected_count=1};
    fixture_count=1;assert(pw_pad_ps5_read(&pad)==PW_OK);
    assert(pad.core.pressed_edges==CREATE && pad.core.previous_buttons==CREATE && !pad.intercepted);
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
    /* The touchpad click keeps the half it began on: the finger's x when
     * pressed, not where it slides, and the middle without a touch. */
    PwPadPs5Data touch={.buttons=TOUCHPAD,.connected=1,.timestamp=5000,.connected_count=1,
                        .touch_data={.fingers=1,.touch={{.x=300,.y=500}}}};
    fixture[0]=touch;fixture_count=1;assert(pw_pad_ps5_read(&pad)==PW_OK);
    assert(pad.touch_held && pad.touch_left && pad.touch_x==300);
    fixture[0].touch_data.touch[0].x=1800;fixture[0].timestamp=5100;
    assert(pw_pad_ps5_read(&pad)==PW_OK && pad.touch_left && pad.touch_x==300);
    fixture[0].buttons=0;fixture[0].timestamp=5200;
    assert(pw_pad_ps5_read(&pad)==PW_OK && !pad.touch_held);
    /* released and pressed again in one batch: the new press's x */
    fixture[0]=touch;fixture[0].touch_data.touch[0].x=959;fixture[1]=touch;fixture[1].buttons=0;
    fixture[2]=touch;fixture[2].touch_data.touch[0].x=960;fixture_count=3;
    assert(pw_pad_ps5_read(&pad)==PW_OK && pad.touch_held && !pad.touch_left && pad.touch_x==960);
    /* an intercepted sample releases it; a press with no touch is the middle, the right */
    fixture[0]=touch;fixture[0].buttons=0x80000000u|TOUCHPAD;fixture_count=1;
    assert(pw_pad_ps5_read(&pad)==PW_OK && !pad.touch_held);
    fixture[0]=touch;fixture[0].touch_data.fingers=0;fixture[0].touch_data.touch[0].x=10;
    assert(pw_pad_ps5_read(&pad)==PW_OK && pad.touch_held && !pad.touch_left && pad.touch_x==960 &&
           pad.touch_from==PW_PAD_PS5_TOUCH_FROM_NONE && !pad.touch_fingers);
    /* A click whose sample has no touch takes the finger's last position
     * before it, while the finger stayed down... */
    fixture[0]=touch;fixture[0].buttons=0;fixture[0].touch_data.touch[0]=(PwPadPs5Touch){.x=120,.y=700};
    fixture[1]=fixture[0];fixture[1].buttons=TOUCHPAD;fixture[1].touch_data.fingers=0;fixture_count=2;
    assert(pw_pad_ps5_read(&pad)==PW_OK && pad.touch_held && pad.touch_left && pad.touch_x==120 &&
           pad.touch_y==700 && pad.touch_from==PW_PAD_PS5_TOUCH_FROM_EARLIER && pad.touch_fingers==1);
    /* ... not once it was lifted; the click's own touch comes first. */
    fixture[0].touch_data.fingers=0;fixture[1].buttons=0;fixture[1].touch_data.fingers=0;
    fixture[2]=fixture[1];fixture[2].buttons=TOUCHPAD;fixture_count=3;
    assert(pw_pad_ps5_read(&pad)==PW_OK && pad.touch_held && !pad.touch_left &&
           pad.touch_from==PW_PAD_PS5_TOUCH_FROM_NONE);
    fixture[0]=touch;fixture[0].buttons=0;fixture[1]=touch;fixture[1].touch_data.touch[0].x=1500;fixture_count=2;
    assert(pw_pad_ps5_read(&pad)==PW_OK && !pad.touch_left && pad.touch_x==1500 && pad.touch_fingers==1 &&
           pad.touch_from==PW_PAD_PS5_TOUCH_FROM_CLICK);
    /* Two fingers make the click (the keyboard's gesture): from its own
     * sample, or from the fingers still down before a sample without one. */
    fixture[0]=touch;fixture[0].buttons=0;fixture_count=1;
    assert(pw_pad_ps5_read(&pad)==PW_OK && !pad.touch_held);
    fixture[0]=touch;fixture[0].touch_data.fingers=2;fixture[0].touch_data.touch[1]=(PwPadPs5Touch){.x=900,.y=400};
    assert(pw_pad_ps5_read(&pad)==PW_OK && pad.touch_held && pad.touch_fingers==2);
    fixture[0].buttons=0;assert(pw_pad_ps5_read(&pad)==PW_OK && !pad.touch_held);
    fixture[0]=touch;fixture[0].buttons=0;fixture[0].touch_data.fingers=2;
    fixture[1]=fixture[0];fixture[1].buttons=TOUCHPAD;fixture[1].touch_data.fingers=0;fixture_count=2;
    assert(pw_pad_ps5_read(&pad)==PW_OK && pad.touch_held && pad.touch_fingers==2 &&
           pad.touch_from==PW_PAD_PS5_TOUCH_FROM_EARLIER);
    /* ... but not once both were lifted before it. */
    fixture[0].buttons=0;fixture[0].touch_data.fingers=0;fixture[1].buttons=0;fixture_count=2;
    assert(pw_pad_ps5_read(&pad)==PW_OK && !pad.touch_held);
    fixture[0]=touch;fixture[0].buttons=0;fixture[0].touch_data.fingers=2;
    fixture[1]=fixture[0];fixture[1].touch_data.fingers=0;
    fixture[2]=fixture[1];fixture[2].buttons=TOUCHPAD;fixture_count=3;
    assert(pw_pad_ps5_read(&pad)==PW_OK && pad.touch_held && !pad.touch_fingers);
    fixture_count=0;
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
    assert(pw_pad_ps5_open_other(&second,&first,-1,map,1)==PW_ERR_NOT_FOUND && !second.opened);
    signed_in[2]=43;
    assert(pw_pad_ps5_open_other(&second,&first,-1,map,1)==PW_OK);
    assert(second.opened && second.user_id==43 && second.pad_handle==8 && !second.owns_user_service);
    assert(open_calls==opened+2);
    fixture[0]=(PwPadPs5Data){.buttons=L1,.connected=1,.timestamp=4000,.connected_count=1};
    fixture_count=1;assert(pw_pad_ps5_read(&second)==PW_OK && second.core.pressed_edges==L1);
    assert(pw_pad_ps5_close(&second)==PW_OK && terminate_calls==terminated);
    /* Three users: the next pick after 43 is 44, then round to 43 again;
     * an id no longer signed in starts from the top. */
    signed_in[3]=44;
    assert(pw_pad_ps5_open_other(&second,&first,43,map,1)==PW_OK && second.user_id==44);
    assert(pw_pad_ps5_close(&second)==PW_OK);
    assert(pw_pad_ps5_open_other(&second,&first,44,map,1)==PW_OK && second.user_id==43);
    assert(pw_pad_ps5_close(&second)==PW_OK);
    assert(pw_pad_ps5_open_other(&second,&first,77,map,1)==PW_OK && second.user_id==43);
    assert(pw_pad_ps5_close(&second)==PW_OK && terminate_calls==terminated);
    signed_in[3]=-1;
    login_rc=-1;assert(pw_pad_ps5_open_other(&second,&first,-1,map,1)==PW_ERR_STATE);login_rc=0;
    first.ops.login_users=NULL;
    assert(pw_pad_ps5_open_other(&second,&first,-1,map,1)==PW_ERR_UNSUPPORTED);
    first.ops.login_users=login_users;
    assert(pw_pad_ps5_close(&first)==PW_OK && terminate_calls==terminated+1);
    assert(pw_pad_ps5_open_other(&second,&first,-1,map,1)==PW_ERR_PRECONDITION);
    return 0;
}
