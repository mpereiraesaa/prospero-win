/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Native Windows API consumer for title XInput users. No renderer or input
 * emulation; stdout is retained by the title's existing fd capture. */
#include <windows.h>
#include <xinput.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef PW_BUILD_ID
#define PW_BUILD_ID "unknown"
#endif

typedef struct {
    XINPUT_CAPABILITIES caps;
    WORD vendor_id,product_id,revision,unknown1;
    DWORD unknown2;
} PwCapsEx;
_Static_assert(sizeof(PwCapsEx)==32,"XInputGetCapabilitiesEx ABI");
typedef DWORD (WINAPI *GetStateFn)(DWORD,XINPUT_STATE *);
typedef DWORD (WINAPI *GetCapsFn)(DWORD,DWORD,XINPUT_CAPABILITIES *);
typedef DWORD (WINAPI *GetCapsExFn)(DWORD,DWORD,DWORD,PwCapsEx *);
static int delta(int a,int b,int step) {return a-b>=step || b-a>=step;}
static int changed(const XINPUT_GAMEPAD *a,const XINPUT_GAMEPAD *b)
{
    return a->wButtons!=b->wButtons || delta(a->bLeftTrigger,b->bLeftTrigger,32) ||
        delta(a->bRightTrigger,b->bRightTrigger,32) || delta(a->sThumbLX,b->sThumbLX,4096) ||
        delta(a->sThumbLY,b->sThumbLY,4096) || delta(a->sThumbRX,b->sThumbRX,4096) ||
        delta(a->sThumbRY,b->sThumbRY,4096);
}
int main(int argc,char **argv)
{
    unsigned seconds=120;
    HMODULE dll;
    GetStateFn get_state;
    GetCapsFn get_caps;
    GetCapsExFn get_caps_ex;
    int connected[4]={-1,-1,-1,-1};
    XINPUT_STATE logged[4]={{0}};
    uint32_t reports[4]={0},connections[4]={0},removals[4]={0};
    unsigned guitar_seen=0,pad_seen=0;
    WORD guitar_buttons=0;
    for(int i=1;i+1<argc;i++)if(!strcmp(argv[i],"--seconds"))seconds=(unsigned)strtoul(argv[i+1],NULL,10);
    if(!seconds || seconds>600)return 2;
    setvbuf(stdout,NULL,_IONBF,0);
    printf("PW_XINPUT_PROBE begin build=%s seconds=%u slots=4\n",PW_BUILD_ID,seconds);
    dll=LoadLibraryA("xinput1_4.dll");
    if(!dll){printf("PW_XINPUT_PROBE error=load-xinput status=%lu\n",GetLastError());return 3;}
    get_state=(GetStateFn)(uintptr_t)GetProcAddress(dll,"XInputGetState");
    get_caps=(GetCapsFn)(uintptr_t)GetProcAddress(dll,"XInputGetCapabilities");
    get_caps_ex=(GetCapsExFn)(uintptr_t)GetProcAddress(dll,(LPCSTR)(uintptr_t)108);
    if(!get_state || !get_caps || !get_caps_ex) {
        printf("PW_XINPUT_PROBE error=missing-api state=%u caps=%u caps_ex=%u\n",get_state!=NULL,get_caps!=NULL,get_caps_ex!=NULL);
        return 4;
    }
    ULONGLONG started=GetTickCount64();
    unsigned tick=0;
    while(GetTickCount64()-started<(ULONGLONG)seconds*1000) {
        for(DWORD index=0;index<4;index++) {
            XINPUT_STATE state={0};
            DWORD rc=get_state(index,&state);
            int present=rc==ERROR_SUCCESS;
            ULONGLONG elapsed=GetTickCount64()-started;
            if(connected[index]!=present || tick%60==0) {
                XINPUT_CAPABILITIES basic={0};
                PwCapsEx extended={0};
                DWORD basic_rc=get_caps(index,0,&basic),ex_rc=get_caps_ex(1,index,0,&extended);
                printf("PW_XINPUT_PROBE caps ms=%llu index=%lu state_rc=%lu caps_rc=%lu ex_rc=%lu type=%u subtype=%u vid=%04x pid=%04x rev=%04x packet=%lu\n",
                       (unsigned long long)elapsed,index,rc,basic_rc,ex_rc,basic.Type,basic.SubType,
                       extended.vendor_id,extended.product_id,extended.revision,state.dwPacketNumber);
                if(present && index==0)pad_seen=1;
                if(present && index==1 && !ex_rc && extended.caps.SubType==7 &&
                   extended.vendor_id==0x1430 && extended.product_id==0x4748)guitar_seen=1;
            }
            if(connected[index]!=present) {
                if(present)connections[index]++;
                else if(connected[index]>0)removals[index]++;
                printf("PW_XINPUT_PROBE connection ms=%llu index=%lu connected=%d\n",
                       (unsigned long long)elapsed,index,present);
            }
            if(present && (!connected[index] || connected[index]<0 || changed(&logged[index].Gamepad,&state.Gamepad))) {
                printf("PW_XINPUT_PROBE state ms=%llu index=%lu packet=%lu buttons=%04x lt=%u rt=%u lx=%d ly=%d rx=%d ry=%d\n",
                       (unsigned long long)elapsed,index,state.dwPacketNumber,state.Gamepad.wButtons,
                       state.Gamepad.bLeftTrigger,state.Gamepad.bRightTrigger,state.Gamepad.sThumbLX,
                       state.Gamepad.sThumbLY,state.Gamepad.sThumbRX,state.Gamepad.sThumbRY);
                logged[index]=state;reports[index]++;
            }
            if(present && index==1)guitar_buttons|=state.Gamepad.wButtons;
            connected[index]=present;
        }
        tick++;Sleep(16);
    }
    printf("PW_XINPUT_PROBE summary pad_seen=%u guitar_seen=%u guitar_buttons=%04x records=%lu/%lu/%lu/%lu connections=%lu/%lu/%lu/%lu removals=%lu/%lu/%lu/%lu\n",
           pad_seen,guitar_seen,guitar_buttons,(unsigned long)reports[0],(unsigned long)reports[1],
           (unsigned long)reports[2],(unsigned long)reports[3],(unsigned long)connections[0],
           (unsigned long)connections[1],(unsigned long)connections[2],(unsigned long)connections[3],
           (unsigned long)removals[0],(unsigned long)removals[1],(unsigned long)removals[2],(unsigned long)removals[3]);
    FreeLibrary(dll);
    return 0;
}
