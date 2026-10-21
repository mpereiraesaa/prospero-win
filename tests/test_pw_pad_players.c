/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../native/pw_pad_ps5.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* The console: the foreground user 42's pad is handle 7, another user's is
 * 100+user. */
static int user_initialize(const void *p){assert(!p);return 0;}
static int foreground(int32_t *u){*u=42;return 0;}
static int terminate(void){return 0;}
static int pad_init(void){return 0;}
static int32_t logged_in[PW_PAD_PS5_USERS]={42,-1,-1,-1};static int login_rc;
static int login_users(int32_t ids[PW_PAD_PS5_USERS]){memcpy(ids,logged_in,sizeof(logged_in));return login_rc;}
static int opens;
static int pad_open(int32_t u,int32_t t,int32_t i,const void *p)
{assert(!t&&!i&&!p);opens++;return u==42?7:100+u;}
static PwPadPs5Data other[1];static int other_count;
static int pad_read(int32_t h,PwPadPs5Data *s,int32_t n)
{assert(n==64&&h!=7);memcpy(s,other,(size_t)other_count*sizeof(*s));return other_count;}
static int closed_handles[8],closed_count;
static int pad_close(int32_t h){if(h!=7)closed_handles[closed_count++&7]=h;return 0;}
static int mode_handles[8],mode_count;
static int vibration_mode(int32_t h,int32_t m)
{assert(m==PW_PAD_PS5_VIBRATION_COMPATIBLE);mode_handles[mode_count++&7]=h;return 0;}
static const PwPadPs5Ops ops={.user_initialize=user_initialize,.foreground_user=foreground,
                              .user_terminate=terminate,.pad_init=pad_init,.pad_open=pad_open,
                              .pad_read=pad_read,.pad_close=pad_close,.login_users=login_users,
                              .pad_set_vibration_mode=vibration_mode};
static const PwPadKeyMap map[]={{0x400,'Z',0,0,"left-flipper"}};

static void sign_in(int32_t a,int32_t b,int32_t c,int32_t d)
{logged_in[0]=a;logged_in[1]=b;logged_in[2]=c;logged_in[3]=d;}

/* The other signed-in users' pads: opened in sign-in order in the compatible
 * vibration mode, kept in their slot, closed when the user signs out; the
 * title's own user is never one. */
int main(void)
{
    PwPadPs5 title;PwPadPlayers players;
    assert(pw_pad_ps5_open(&title,&ops,map,1)==PW_OK);
    pw_pad_players_init(&players);
    assert(players.slot[0].user_id==-1 && players.slot[0].pad_handle==-1);
    assert(pw_pad_players_scan(&players,&title)==0 && !players.slot[0].opened);
    sign_in(5,42,9,-1);
    int modes=mode_count;
    assert(pw_pad_players_scan(&players,&title)==2);
    assert(players.slot[0].opened && players.slot[0].user_id==5 && players.slot[0].pad_handle==105);
    assert(players.slot[1].opened && players.slot[1].user_id==9 && !players.slot[2].opened);
    assert(!players.slot[0].owns_user_service);
    assert(mode_count==modes+2 && mode_handles[modes&7]==105 && mode_handles[(modes+1)&7]==109);
    int before=opens;
    assert(pw_pad_players_scan(&players,&title)==2 && opens==before);   /* already open */
    /* 5 signs out: 9 keeps slot 1, and a new user takes the free slot 0. */
    sign_in(42,9,-1,-1);
    assert(pw_pad_players_scan(&players,&title)==1 && !players.slot[0].opened &&
           closed_count==1 && closed_handles[0]==105 && players.slot[1].user_id==9);
    sign_in(42,9,3,-1);
    assert(pw_pad_players_scan(&players,&title)==2 && players.slot[0].user_id==3);
    /* A player's pad reads as the title's does. */
    other[0]=(PwPadPs5Data){.buttons=0x4000,.left_stick={1,2},.connected=1,.timestamp=10,.connected_count=1};
    other_count=1;
    assert(pw_pad_ps5_read(&players.slot[1])==PW_OK && players.slot[1].core.connected &&
           players.slot[1].core.previous_buttons==0x4000 && players.slot[1].left_stick.x==1);
    other_count=0;
    /* A failed sign-in list changes nothing; without the op, no players. */
    login_rc=-1;assert(pw_pad_players_scan(&players,&title)==2);login_rc=0;
    PwPadPs5 bare=title;bare.ops.login_users=NULL;
    PwPadPlayers none;pw_pad_players_init(&none);
    assert(pw_pad_players_scan(&none,&bare)==0 && !none.slot[0].opened);
    /* A closed title pad opens no players. */
    PwPadPs5 shut=title;shut.opened=0;
    assert(pw_pad_players_scan(&none,&shut)==0);
    pw_pad_players_close(&players);
    assert(!players.slot[0].opened && !players.slot[1].opened && closed_count==3);
    assert(pw_pad_ps5_close(&title)==PW_OK);
    puts("pad players passed: sign-in order, kept slots, sign-out and reuse, compatible vibration mode, reads, failed lists");
    return 0;
}
