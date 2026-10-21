/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_pad_ps5.h"
#include <limits.h>
#include <stddef.h>
#include <string.h>

enum { PW_PAD_INTERCEPTED=0x80000000u };
_Static_assert(sizeof(PwPadPs5Data)==120,"ScePad data ABI");
_Static_assert(offsetof(PwPadPs5Data,connected)==76,"ScePad connected offset");
_Static_assert(offsetof(PwPadPs5Data,timestamp)==80,"ScePad timestamp offset");
_Static_assert(offsetof(PwPadPs5Data,connected_count)==104,"ScePad generation offset");

#ifndef PW_PAD_PS5_HOST_TEST
extern int sceUserServiceInitialize(const void *);
extern int sceUserServiceGetForegroundUser(int32_t *);
extern int sceUserServiceTerminate(void);
extern int scePadInit(void);
extern int scePadOpen(int32_t,int32_t,int32_t,const void *);
extern int scePadRead(int32_t,PwPadPs5Data *,int32_t);
extern int scePadClose(int32_t);
extern int scePadSetVibration(int32_t,const uint8_t *);
extern int scePadSetVibrationMode(int32_t,int32_t);
extern int sceUserServiceGetLoginUserIdList(int32_t *);
static int platform_user_initialize(const void *p){return sceUserServiceInitialize(p);}
static int platform_foreground_user(int32_t *u){return sceUserServiceGetForegroundUser(u);}
static int platform_user_terminate(void){return sceUserServiceTerminate();}
static int platform_pad_init(void){return scePadInit();}
static int platform_pad_open(int32_t u,int32_t t,int32_t i,const void *p)
{return scePadOpen(u,t,i,p);}
static int platform_pad_read(int32_t h,PwPadPs5Data *s,int32_t n)
{return scePadRead(h,s,n);}
static int platform_pad_close(int32_t h){return scePadClose(h);}
static int platform_pad_set_vibration(int32_t h,const uint8_t m[2]){return scePadSetVibration(h,m);}
static int platform_login_users(int32_t ids[PW_PAD_PS5_USERS])
{return sceUserServiceGetLoginUserIdList(ids);}
static int platform_pad_set_vibration_mode(int32_t h,int32_t m){return scePadSetVibrationMode(h,m);}
#endif

int pw_pad_ps5_platform_ops(PwPadPs5Ops *ops)
{
    if(!ops)return PW_ERR_PRECONDITION;
#ifdef PW_PAD_PS5_HOST_TEST
    memset(ops,0,sizeof(*ops));return PW_ERR_UNSUPPORTED;
#else
    *ops=(PwPadPs5Ops){platform_user_initialize,platform_foreground_user,
        platform_user_terminate,platform_pad_init,platform_pad_open,
        platform_pad_read,platform_pad_close,platform_pad_set_vibration,
        platform_login_users,platform_pad_set_vibration_mode};return PW_OK;
#endif
}

static int valid_ops(const PwPadPs5Ops *ops)
{
    return ops && ops->user_initialize && ops->foreground_user && ops->user_terminate &&
        ops->pad_init && ops->pad_open && ops->pad_read && ops->pad_close;
}

static void centre_analog(PwPadPs5 *pad)
{
    pad->left_stick=pad->right_stick=(PwPadPs5Stick){0x80,0x80};pad->l2=pad->r2=0;
}

int pw_pad_ps5_open(PwPadPs5 *pad,const PwPadPs5Ops *ops,
                    const PwPadKeyMap *map,size_t map_count)
{
    if(!pad || !valid_ops(ops))return PW_ERR_PRECONDITION;
    memset(pad,0,sizeof(*pad));pad->ops=*ops;pad->user_id=-1;pad->pad_handle=-1;
    pad->close_rc=INT_MIN;pad->terminate_rc=INT_MIN;
    int status=pw_pad_init(&pad->core,map,map_count);if(status!=PW_OK)return status;
    pad->user_initialize_rc=pad->ops.user_initialize(NULL);
    pad->owns_user_service=pad->user_initialize_rc==0;
    int rc=pad->ops.foreground_user(&pad->user_id);
    if(rc<0 || pad->user_id<0)goto failed;
    pad->pad_init_rc=pad->ops.pad_init();if(pad->pad_init_rc<0)goto failed;
    pad->pad_handle=pad->ops.pad_open(pad->user_id,0,0,NULL);
    if(pad->pad_handle<0)goto failed;
    pad->opened=1;centre_analog(pad);
    if(pad->ops.pad_set_vibration_mode)
        pad->ops.pad_set_vibration_mode(pad->pad_handle,PW_PAD_PS5_VIBRATION_COMPATIBLE);
    return PW_OK;
failed:
    if(pad->owns_user_service){pad->terminate_rc=pad->ops.user_terminate();pad->owns_user_service=0;}
    return PW_ERR_STATE;
}

int pw_pad_ps5_open_other(PwPadPs5 *pad,const PwPadPs5 *first,int32_t after,
                          const PwPadKeyMap *map,size_t map_count)
{
    int32_t ids[PW_PAD_PS5_USERS];
    if(!pad || !first || !first->opened || !valid_ops(&first->ops))return PW_ERR_PRECONDITION;
    memset(pad,0,sizeof(*pad));pad->ops=first->ops;pad->user_id=-1;pad->pad_handle=-1;
    pad->close_rc=INT_MIN;pad->terminate_rc=INT_MIN;
    if(!pad->ops.login_users)return PW_ERR_UNSUPPORTED;
    int status=pw_pad_init(&pad->core,map,map_count);if(status!=PW_OK)return status;
    for(int i=0;i<PW_PAD_PS5_USERS;i++)ids[i]=-1;
    if(pad->ops.login_users(ids)<0)return PW_ERR_STATE;
    int start=0;
    for(int i=0;i<PW_PAD_PS5_USERS;i++)if(after>=0 && ids[i]==after){start=i+1;break;}
    for(int k=0;k<PW_PAD_PS5_USERS && pad->user_id<0;k++) {
        int i=(start+k)%PW_PAD_PS5_USERS;
        if(ids[i]>=0 && ids[i]!=first->user_id)pad->user_id=ids[i];
    }
    if(pad->user_id<0)return PW_ERR_NOT_FOUND;
    pad->pad_handle=pad->ops.pad_open(pad->user_id,0,0,NULL);
    if(pad->pad_handle<0)return PW_ERR_STATE;
    pad->opened=1;centre_analog(pad);return PW_OK;
}

/* One native read converted to samples; count<0 reports a read error. */
static int read_batch(PwPadPs5 *pad,PwPadSample samples[PW_PAD_PS5_BATCH])
{
    PwPadPs5Data raw[PW_PAD_PS5_BATCH];pad->polls++;
    /* Edge fields describe exactly one native read, including an empty one.
     * Without this reset a Create press followed by an empty read could be
     * consumed repeatedly by the title lifecycle adapter. */
    pad->core.pressed_edges=0;pad->core.released_edges=0;
    int count=pad->ops.pad_read(pad->pad_handle,raw,PW_PAD_PS5_BATCH);pad->last_read_rc=count;
    if(count<0){pad->read_errors++;centre_analog(pad);return count;}
    if(!count){pad->empty_reads++;return 0;}
    if(count>PW_PAD_PS5_BATCH)count=PW_PAD_PS5_BATCH;
    for(int i=0;i<count;i++) {
        unsigned intercepted=(raw[i].buttons&PW_PAD_INTERCEPTED)!=0;
        samples[i]=(PwPadSample){.buttons=raw[i].buttons&~PW_PAD_INTERCEPTED,
            .timestamp_us=raw[i].timestamp,.generation=raw[i].connected_count,
            .connected=raw[i].connected!=0,.intercepted=intercepted};
        if(!pad->generation_valid || pad->last_generation!=raw[i].connected_count) {
            pad->last_generation=raw[i].connected_count;pad->generation_valid=1;
            pad->generation_changes++;
        }
        if(intercepted){pad->intercepted_samples++;centre_analog(pad);}
        else if(raw[i].connected) {
            pad->connected_samples++;
            pad->left_stick=raw[i].left_stick;pad->right_stick=raw[i].right_stick;
            pad->l2=raw[i].l2;pad->r2=raw[i].r2;
        }
        else{pad->disconnected_samples++;centre_analog(pad);}
    }
    return count;
}

int pw_pad_ps5_read(PwPadPs5 *pad)
{
    if(!pad || !pad->opened)return PW_ERR_PRECONDITION;
    PwPadSample samples[PW_PAD_PS5_BATCH];int count=read_batch(pad,samples);
    if(count<0) {
        const PwPadSample released={.connected=0};
        return pw_pad_track(&pad->core,&released,1);
    }
    if(!count)return PW_OK;
    return pw_pad_track(&pad->core,samples,(size_t)count);
}

int pw_pad_ps5_close(PwPadPs5 *pad)
{
    if(!pad)return PW_ERR_PRECONDITION;
    int status=PW_OK;
    if(pad->opened) {
        pad->close_rc=pad->ops.pad_close(pad->pad_handle);pad->opened=0;pad->pad_handle=-1;
        if(pad->close_rc<0)status=PW_ERR_STATE;
    }
    if(pad->owns_user_service) {
        pad->terminate_rc=pad->ops.user_terminate();pad->owns_user_service=0;
        if(pad->terminate_rc<0)status=PW_ERR_STATE;
    }
    return status;
}

void pw_pad_players_init(PwPadPlayers *players)
{
    if(!players)return;
    memset(players,0,sizeof(*players));
    for(int i=0;i<PW_PAD_PLAYERS;i++){players->slot[i].user_id=-1;players->slot[i].pad_handle=-1;}
}

static int signed_in(const int32_t ids[PW_PAD_PS5_USERS],int32_t user)
{
    for(int i=0;i<PW_PAD_PS5_USERS;i++)if(ids[i]==user)return 1;
    return 0;
}

/* Another user's pad: the user service and scePadInit are the title pad's. */
static int open_player(PwPadPs5 *pad,const PwPadPs5 *title_pad,int32_t user)
{
    memset(pad,0,sizeof(*pad));pad->ops=title_pad->ops;pad->user_id=user;pad->pad_handle=-1;
    pad->close_rc=INT_MIN;pad->terminate_rc=INT_MIN;
    if(pw_pad_init(&pad->core,title_pad->core.map,title_pad->core.map_count)!=PW_OK)goto failed;
    pad->pad_handle=pad->ops.pad_open(user,0,0,NULL);
    if(pad->pad_handle<0)goto failed;
    pad->opened=1;centre_analog(pad);
    if(pad->ops.pad_set_vibration_mode)
        pad->ops.pad_set_vibration_mode(pad->pad_handle,PW_PAD_PS5_VIBRATION_COMPATIBLE);
    return 1;
failed:
    pad->user_id=-1;pad->pad_handle=-1;return 0;
}

int pw_pad_players_scan(PwPadPlayers *players,const PwPadPs5 *title_pad)
{
    int open=0;
    if(!players || !title_pad)return 0;
    int32_t ids[PW_PAD_PS5_USERS];
    for(int i=0;i<PW_PAD_PS5_USERS;i++)ids[i]=-1;
    if(title_pad->opened && title_pad->ops.login_users && title_pad->ops.login_users(ids)>=0) {
        for(int i=0;i<PW_PAD_PLAYERS;i++) {
            PwPadPs5 *pad=&players->slot[i];
            if(pad->opened && !signed_in(ids,pad->user_id)) {
                (void)pw_pad_ps5_close(pad);pad->user_id=-1;
            }
        }
        for(int u=0;u<PW_PAD_PS5_USERS;u++) {
            int32_t user=ids[u];int known=user<0 || user==title_pad->user_id;
            for(int i=0;!known && i<PW_PAD_PLAYERS;i++)
                known=players->slot[i].opened && players->slot[i].user_id==user;
            for(int i=0;!known && i<PW_PAD_PLAYERS;i++)
                if(!players->slot[i].opened){(void)open_player(&players->slot[i],title_pad,user);known=1;}
        }
    }
    for(int i=0;i<PW_PAD_PLAYERS;i++)open+=players->slot[i].opened!=0;
    return open;
}

void pw_pad_players_close(PwPadPlayers *players)
{
    if(!players)return;
    for(int i=0;i<PW_PAD_PLAYERS;i++)
        if(players->slot[i].opened){(void)pw_pad_ps5_close(&players->slot[i]);players->slot[i].user_id=-1;}
}

int pw_pad_ps5_vibrate(PwPadPs5 *pad,uint8_t large,uint8_t small)
{
    if(!pad || !pad->opened)return PW_ERR_PRECONDITION;
    if(!pad->ops.pad_set_vibration)return PW_ERR_UNSUPPORTED;
    const uint8_t motors[2]={large,small};
    pad->vibration_rc=pad->ops.pad_set_vibration(pad->pad_handle,motors);
    return pad->vibration_rc<0?PW_ERR_STATE:PW_OK;
}
