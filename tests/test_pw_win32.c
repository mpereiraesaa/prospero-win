/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_win32.h"
#include "../src/pw_vm_posix.h"
#include <assert.h>
#include <string.h>
static int string_fixture(void *opaque,uint32_t module,uint32_t id,const uint8_t **text,size_t *units)
{
    (void)opaque;
    static const uint8_t sample[]={'A',0,0xe9,0,0xac,0x20,0x14,0x20};
    static const uint8_t unmapped[]={0,0x4e};
    if(module!=0x01000000)return PW_ERR_UNSUPPORTED;
    if(id==3)return PW_ERR_NOT_FOUND;
    if(id==4)return PW_ERR_TRUNCATED;
    *text=id==2?unmapped:sample;*units=id==1?0:id==2?1:4;return PW_OK;
}
static int named_fixture(void *opaque,uint32_t module,uint32_t type,const char *name,
                         const uint8_t **bytes,size_t *size)
{
    (void)opaque;static const uint8_t icon[]={1,2,3,4};
    if(module!=0x01000000 || type!=14)return PW_ERR_UNSUPPORTED;
    if(!strcmp(name,"MISSING"))return PW_ERR_NOT_FOUND;
    if(strcmp(name,"ICON_1"))return PW_ERR_MALFORMED;
    *bytes=icon;*size=sizeof(icon);return PW_OK;
}
static int code_fixture(void *opaque,uint32_t address)
{
    (void)opaque;return address==0x01002000?PW_OK:PW_ERR_NOT_FOUND;
}
static int profile_fixture(void *opaque,const char *section,const char *key,
                           uint32_t fallback,const char *filename,uint32_t *value)
{
    (void)opaque;
    if(strcmp(section,"WinNT:default") || strcmp(key,"WaveBlocks") ||
       strcmp(filename,"C:\\game\\wavemix.inf") || fallback!=3)return PW_ERR_MALFORMED;
    *value=5;return PW_OK;
}
static int null_message_fixture(void *opaque,uint32_t window,PwUser32QueueEntry *message)
{
    (void)opaque;
    if(window!=0x10001 || !message)return PW_ERR_MALFORMED;
    *message=(PwUser32QueueEntry){.window=window,.message=0}; /* WM_NULL */
    return PW_OK;
}
typedef struct AudioFixture {
    uint32_t token,bytes;
    unsigned submissions,ready;
    int result;
} AudioFixture;
static int audio_submit_fixture(void *opaque,const void *pcm,uint32_t bytes,uint32_t token)
{
    AudioFixture *audio=opaque;
    if(!audio || !pcm || !bytes || !token)return PW_ERR_PRECONDITION;
    if(audio->result)return audio->result;
    audio->token=token;audio->bytes=bytes;audio->submissions++;return PW_OK;
}
static int audio_poll_fixture(void *opaque,uint32_t *token,uint32_t *bytes)
{
    AudioFixture *audio=opaque;
    if(!audio || !token || !bytes)return PW_ERR_PRECONDITION;
    if(!audio->ready)return PW_ERR_NOT_FOUND;
    *token=audio->token;*bytes=audio->bytes;audio->ready=0;return PW_OK;
}
static void profile_tests(PwWin32 *r,PwX86State *s)
{
    PeImportSymbol symbol={0};PwImportTarget target;strcpy(symbol.name,"GetPrivateProfileIntA");
    assert(pw_win32_resolve(r,"kernel32.dll",&symbol,&target)==PW_OK);
    uint32_t text=s->stack_low+256;
    strcpy((char *)(uintptr_t)text,"WinNT:default");
    strcpy((char *)(uintptr_t)(text+32),"WaveBlocks");
    strcpy((char *)(uintptr_t)(text+64),"C:\\game\\wavemix.inf");
    s->eip=(uint32_t)target.address;s->gpr[4]=s->stack_high-20;s->eflags=0xad7;
    uint32_t frame[]={0x01001234,text,text+32,3,text+64};
    memcpy((void *)(uintptr_t)s->gpr[4],frame,sizeof(frame));
    PwX86State before=*s;unsigned calls=r->calls;
    assert(pw_win32_dispatch(r,s)==PW_ERR_STATE && !memcmp(s,&before,sizeof(before)) && r->calls==calls);
    r->services.profile_int=profile_fixture;
    assert(pw_win32_dispatch(r,s)==PW_OK && s->gpr[0]==5 && s->eip==frame[0] &&
           s->gpr[4]==s->stack_high && s->eflags==0xad7 && r->calls==calls+1);
    s->eip=(uint32_t)target.address;s->gpr[4]=s->stack_high-20;frame[1]=0;
    memcpy((void *)(uintptr_t)s->gpr[4],frame,sizeof(frame));before=*s;calls=r->calls;
    assert(pw_win32_dispatch(r,s)==PW_ERR_VM && !memcmp(s,&before,sizeof(before)) && r->calls==calls);
}
static void waveout_tests(PwWin32 *r,PwX86State *s)
{
    PeImportSymbol symbol={0};PwImportTarget target;uint32_t ret=0x01001234;
    strcpy(symbol.name,"waveOutGetNumDevs");
    assert(pw_win32_resolve(r,"winmm.dll",&symbol,&target)==PW_OK);
    s->eip=(uint32_t)target.address;s->gpr[4]=s->stack_high-4;s->eflags=0xad7;
    memcpy((void *)(uintptr_t)s->gpr[4],&ret,4);
    assert(pw_win32_dispatch(r,s)==PW_OK && s->gpr[0]==1 && s->gpr[4]==s->stack_high && s->eflags==0xad7);

    uint32_t data=s->stack_low+512;memset((void *)(uintptr_t)data,0xcc,128);
    strcpy(symbol.name,"waveOutGetDevCapsA");
    assert(pw_win32_resolve(r,"winmm.dll",&symbol,&target)==PW_OK);
    s->eip=(uint32_t)target.address;s->gpr[4]=s->stack_high-16;
    uint32_t caps[]={ret,0,data,52};memcpy((void *)(uintptr_t)s->gpr[4],caps,sizeof(caps));
    assert(pw_win32_dispatch(r,s)==PW_OK && !s->gpr[0] && !strcmp((char *)(uintptr_t)(data+8),"Prospero PCM"));
    assert(*(uint16_t *)(uintptr_t)(data+44)==2 && *(uint8_t *)(uintptr_t)(data+52)==0xcc);

    uint8_t format[18]={1,0,1,0,0x11,0x2b,0,0,0x11,0x2b,0,0,1,0,8,0,0,0};
    memcpy((void *)(uintptr_t)(data+64),format,sizeof(format));
    strcpy(symbol.name,"waveOutOpen");
    assert(pw_win32_resolve(r,"winmm.dll",&symbol,&target)==PW_OK);
    uint32_t open[]={ret,data+96,UINT32_MAX,data+64,0x10005,0,0x10000};
    s->eip=(uint32_t)target.address;s->gpr[4]=s->stack_high-28;
    memcpy((void *)(uintptr_t)s->gpr[4],open,sizeof(open));
    assert(pw_win32_dispatch(r,s)==PW_OK && !s->gpr[0] &&
           *(uint32_t *)(uintptr_t)(data+96)==0x0e000001 && r->wave_out.open &&
           r->wave_out.samples_per_second==11025 && r->wave_out.bits_per_sample==8);
    uint32_t header=data+104;memset((void *)(uintptr_t)header,0,32);
    *(uint32_t *)(uintptr_t)header=data;*(uint32_t *)(uintptr_t)(header+4)=64;
    strcpy(symbol.name,"waveOutPrepareHeader");
    assert(pw_win32_resolve(r,"winmm.dll",&symbol,&target)==PW_OK);
    uint32_t prepare[]={ret,0x0e000001,header,32};
    s->eip=(uint32_t)target.address;s->gpr[4]=s->stack_high-16;
    memcpy((void *)(uintptr_t)s->gpr[4],prepare,sizeof(prepare));
    assert(pw_win32_dispatch(r,s)==PW_OK && !s->gpr[0] &&
           *(uint32_t *)(uintptr_t)(header+16)==2 && r->wave_out.header_count==1);
    AudioFixture audio={0};r->services.opaque=&audio;
    r->services.audio_submit=audio_submit_fixture;r->services.audio_poll=audio_poll_fixture;
    strcpy(symbol.name,"waveOutWrite");
    assert(pw_win32_resolve(r,"winmm.dll",&symbol,&target)==PW_OK);
    uint32_t write[]={ret,0x0e000001,header,32};
    s->eip=(uint32_t)target.address;s->gpr[4]=s->stack_high-16;
    memcpy((void *)(uintptr_t)s->gpr[4],write,sizeof(write));
    assert(pw_win32_dispatch(r,s)==PW_OK && !s->gpr[0] && audio.submissions==1 &&
           audio.token==header && audio.bytes==64 &&
           *(uint32_t *)(uintptr_t)(header+16)==0x12);
    uint32_t completed=99;
    assert(pw_win32_pump_audio(r,s,&completed)==PW_OK && !completed &&
           *(uint32_t *)(uintptr_t)(header+16)==0x12);
    audio.ready=1;
    assert(pw_win32_pump_audio(r,s,&completed)==PW_OK && completed==1 &&
           *(uint32_t *)(uintptr_t)(header+16)==3);
    audio.result=PW_ERR_LIMIT;
    s->eip=(uint32_t)target.address;s->gpr[4]=s->stack_high-16;
    memcpy((void *)(uintptr_t)s->gpr[4],write,sizeof(write));
    assert(pw_win32_dispatch(r,s)==PW_OK && s->gpr[0]==7 &&
           *(uint32_t *)(uintptr_t)(header+16)==3);
    r->services.opaque=NULL;r->services.audio_submit=NULL;r->services.audio_poll=NULL;
    uint32_t position=data+160;memset((void *)(uintptr_t)position,0xcc,12);
    *(uint32_t *)(uintptr_t)position=4;r->wave_out.bytes_completed=12345;
    strcpy(symbol.name,"waveOutGetPosition");
    assert(pw_win32_resolve(r,"winmm.dll",&symbol,&target)==PW_OK);
    uint32_t get_position[]={ret,0x0e000001,position,12};
    s->eip=(uint32_t)target.address;s->gpr[4]=s->stack_high-16;
    memcpy((void *)(uintptr_t)s->gpr[4],get_position,sizeof(get_position));
    assert(pw_win32_dispatch(r,s)==PW_OK && !s->gpr[0] &&
           *(uint32_t *)(uintptr_t)position==4 && *(uint32_t *)(uintptr_t)(position+4)==12345 &&
           *(uint32_t *)(uintptr_t)(position+8)==0);
    r->wave_out.bytes_submitted=0;r->wave_out.bytes_completed=0;
    PwWaveOut before=r->wave_out;open[6]=1;open[1]=data+100;
    strcpy(symbol.name,"waveOutOpen");
    assert(pw_win32_resolve(r,"winmm.dll",&symbol,&target)==PW_OK);
    s->eip=(uint32_t)target.address;s->gpr[4]=s->stack_high-28;
    memcpy((void *)(uintptr_t)s->gpr[4],open,sizeof(open));
    assert(pw_win32_dispatch(r,s)==PW_OK && !s->gpr[0] &&
           !memcmp(&before,&r->wave_out,sizeof(before)) && *(uint32_t *)(uintptr_t)(data+100)==0xcccccccc);

    strcpy(symbol.name,"mciSendCommandA");
    assert(pw_win32_resolve(r,"winmm.dll",&symbol,&target)==PW_OK);
    uint32_t mci[]={ret,0,0x803,0x2000,0};
    s->eip=(uint32_t)target.address;s->gpr[4]=s->stack_high-20;s->eflags=0xad7;
    memcpy((void *)(uintptr_t)s->gpr[4],mci,sizeof(mci));
    assert(pw_win32_dispatch(r,s)==PW_OK && s->gpr[0]==0x107 &&
           s->eip==ret && s->gpr[4]==s->stack_high && s->eflags==0xad7 &&
           r->mci_calls==1 && r->mci_last_command==0x803);

    /* mmioClose has exactly two stdcall arguments.  An earlier three-argument
     * model silently consumed the caller's saved ESI and nulled every loaded
     * WavePtr when the original WaveMix routine returned. */
    strcpy(symbol.name,"mmioClose");
    assert(pw_win32_resolve(r,"winmm.dll",&symbol,&target)==PW_OK);
    uint32_t close_missing[]={ret,0x0f00ffff,0};
    s->eip=(uint32_t)target.address;s->gpr[4]=s->stack_high-sizeof(close_missing);
    s->eflags=0xad7;memcpy((void *)(uintptr_t)s->gpr[4],close_missing,sizeof(close_missing));
    assert(pw_win32_dispatch(r,s)==PW_OK && s->gpr[0]==UINT32_MAX &&
           s->eip==ret && s->gpr[4]==s->stack_high && s->eflags==0xad7);
}
static void resource_tests(PwWin32 *r,PwX86State *s)
{
    PeImportSymbol symbol={0};PwImportTarget target;
    uint32_t name=s->stack_low+64;strcpy((char *)(uintptr_t)name,"ICON_1");
    const char *apis[]={"LoadIconA","LoadIconA","LoadCursorA"};
    for(unsigned i=0;i<3;i++) {
        strcpy(symbol.name,apis[i]);assert(pw_win32_resolve(r,"user32.dll",&symbol,&target)==PW_OK);
        s->eip=(uint32_t)target.address;s->gpr[4]=s->stack_high-12;s->eflags=0xad7;
        uint32_t frame[]={0x01001234,i<2?0x01000000:0,i<2?name:32512};
        memcpy((void *)(uintptr_t)s->gpr[4],frame,sizeof(frame));unsigned calls=r->calls;
        assert(pw_win32_dispatch(r,s)==PW_OK && s->eip==frame[0] &&
               s->gpr[4]==s->stack_high && s->eflags==0xad7 && r->calls==calls+1);
        assert(s->gpr[0]==(i<2?PW_USER32_OBJECT_FIRST:PW_USER32_OBJECT_FIRST+1));
    }
    strcpy((char *)(uintptr_t)name,"MISSING");strcpy(symbol.name,"LoadIconA");
    assert(pw_win32_resolve(r,"user32.dll",&symbol,&target)==PW_OK);
    s->eip=(uint32_t)target.address;s->gpr[4]=s->stack_high-12;
    uint32_t missing[]={0x01001234,0x01000000,name};
    memcpy((void *)(uintptr_t)s->gpr[4],missing,sizeof(missing));
    assert(pw_win32_dispatch(r,s)==PW_OK && !s->gpr[0] && r->last_error==1814);
    unsigned used=0;for(unsigned i=0;i<r->user32->resource_capacity;i++)used+=r->user32->resources[i].used;
    assert(used==2);
    strcpy((char *)(uintptr_t)name,"ICON_1");s->eip=(uint32_t)target.address;
    s->gpr[4]=s->stack_high-8;PwX86State before=*s;unsigned calls=r->calls;
    assert(pw_win32_dispatch(r,s)==PW_ERR_VM && !memcmp(s,&before,sizeof(before)) && r->calls==calls);
    used=0;for(unsigned i=0;i<r->user32->resource_capacity;i++)used+=r->user32->resources[i].used;
    assert(used==2);
}
static void class_tests(PwWin32 *r,PwX86State *s)
{
    PeImportSymbol symbol={0};PwImportTarget target;strcpy(symbol.name,"RegisterClassA");
    assert(pw_win32_resolve(r,"user32.dll",&symbol,&target)==PW_OK);
    uint32_t base=s->stack_low+128,raw[10]={4104,0x01002000,0,0,0x01000000,
        PW_USER32_OBJECT_FIRST,PW_USER32_OBJECT_FIRST+1,16,base+48,base+64};
    memcpy((void *)(uintptr_t)base,raw,sizeof(raw));
    strcpy((char *)(uintptr_t)(base+48),"MENU_1");strcpy((char *)(uintptr_t)(base+64),"Pinball");
    s->eip=(uint32_t)target.address;s->gpr[4]=s->stack_high-4;
    PwX86State before=*s;unsigned calls=r->calls;
    assert(pw_win32_dispatch(r,s)==PW_ERR_VM && !memcmp(s,&before,sizeof(before)) &&
           r->calls==calls && !r->user32->classes[0].used);
    for(unsigned attempt=0;attempt<2;attempt++) {
        s->eip=(uint32_t)target.address;s->gpr[4]=s->stack_high-8;
        uint32_t frame[]={0x01001234,base};memcpy((void *)(uintptr_t)s->gpr[4],frame,sizeof(frame));
        calls=r->calls;assert(pw_win32_dispatch(r,s)==PW_OK && s->eip==frame[0] &&
            s->gpr[4]==s->stack_high && r->calls==calls+1);
        assert(s->gpr[0]==(attempt?0:PW_USER32_ATOM_FIRST));
        if(attempt)assert(r->last_error==1410);
    }
    assert(r->user32->classes[0].used && r->user32->classes[0].wndproc==0x01002000 &&
           !strcmp(r->user32->classes[0].class_name,"Pinball"));
}
static void string_tests(PwWin32 *r,PwX86State *s)
{
    PeImportSymbol symbol={0};PwImportTarget target;strcpy(symbol.name,"LoadStringA");
    assert(pw_win32_resolve(r,"user32.dll",&symbol,&target)==PW_OK);
    r->services.string_resource=string_fixture;r->services.ansi_codepage=1252;
    uint32_t output=s->stack_low+1;
    const uint8_t converted[]={'A',0xe9,0x80,0x97};
    for(unsigned id=0;id<5;id++)for(unsigned cap=0;cap<7;cap++) {
        memset((void *)(uintptr_t)(output-1),0xcc,12);
        s->eip=(uint32_t)target.address;s->gpr[4]=s->stack_high-20;s->eflags=0xad7;
        uint32_t frame[]={0x01001234,0x01000000,id,output,cap};
        memcpy((void *)(uintptr_t)s->gpr[4],frame,sizeof(frame));PwX86State before=*s;
        int status=pw_win32_dispatch(r,s);
        if(!cap || id==4 || (id==2 && cap>1)) {
            assert(status==(!cap || id==2?PW_ERR_UNSUPPORTED:PW_ERR_TRUNCATED));
            assert(!memcmp(s,&before,sizeof(before)) && *(uint8_t *)(uintptr_t)output==0xcc);
        } else {
            unsigned n=id==0?(cap-1<4?cap-1:4):0;
            assert(status==PW_OK && s->gpr[0]==n && s->eip==frame[0] && s->gpr[4]==s->stack_high && s->eflags==0xad7);
            if(id==3)assert(*(uint8_t *)(uintptr_t)output==0xcc);
            else assert(!memcmp((void *)(uintptr_t)output,converted,n) && *(uint8_t *)(uintptr_t)(output+n)==0);
            assert(*(uint8_t *)(uintptr_t)(output-1)==0xcc && *(uint8_t *)(uintptr_t)(output+n+1)==0xcc);
        }
    }
    s->gpr[4]=s->stack_high-64;s->eip=(uint32_t)target.address;
    uint32_t bad[]={0x01001234,0x01000000,0,s->stack_high-4,5};
    memcpy((void *)(uintptr_t)s->gpr[4],bad,sizeof(bad));PwX86State before=*s;
    assert(pw_win32_dispatch(r,s)==PW_ERR_VM && !memcmp(s,&before,sizeof(before)));
    bad[3]=output;bad[4]=4097;memcpy((void *)(uintptr_t)s->gpr[4],bad,sizeof(bad));
    assert(pw_win32_dispatch(r,s)==PW_ERR_UNSUPPORTED && !memcmp(s,&before,sizeof(before)));
    r->services.ansi_codepage=65001;
    assert(pw_win32_dispatch(r,s)==PW_ERR_UNSUPPORTED);
}
static void itoa_tests(PwWin32 *r,PwX86State *s)
{
    PeImportSymbol symbol={0};PwImportTarget target;strcpy(symbol.name,"_itoa");
    assert(pw_win32_resolve(r,"msvcrt.dll",&symbol,&target)==PW_OK);
    uint32_t output=s->stack_low+400;
    const struct {uint32_t value,radix;const char *expected;} cases[]={
        {0,10,"0"},{(uint32_t)-123,10,"-123"},{UINT32_MAX,16,"ffffffff"},
        {35,36,"z"},{UINT32_MAX,2,"11111111111111111111111111111111"}
    };
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        memset((void *)(uintptr_t)output,0xcc,40);
        uint32_t frame[]={0x01001234,cases[i].value,output,cases[i].radix};
        s->eip=(uint32_t)target.address;s->gpr[4]=s->stack_high-sizeof(frame);s->eflags=0xad7;
        memcpy((void *)(uintptr_t)s->gpr[4],frame,sizeof(frame));
        assert(pw_win32_dispatch(r,s)==PW_OK && s->gpr[0]==output &&
               s->eip==frame[0] && s->gpr[4]==s->stack_high-12 && s->eflags==0xad7 &&
               !strcmp((char *)(uintptr_t)output,cases[i].expected));
    }
    uint32_t bad[]={0x01001234,1,output,1};
    s->eip=(uint32_t)target.address;s->gpr[4]=s->stack_high-sizeof(bad);
    memcpy((void *)(uintptr_t)s->gpr[4],bad,sizeof(bad));PwX86State before=*s;
    assert(pw_win32_dispatch(r,s)==PW_ERR_UNSUPPORTED && !memcmp(s,&before,sizeof(before)));
}
static uint32_t heap_call(PwWin32 *r,PwX86State *s,const char *name,uint32_t a,uint32_t b,int expected)
{
    PeImportSymbol symbol={0};PwImportTarget target;strcpy(symbol.name,name);
    assert(pw_win32_resolve(r,"msvcrt.dll",&symbol,&target)==PW_OK);
    s->eip=(uint32_t)target.address;s->gpr[4]=s->stack_high-12;s->gpr[0]=0xaabbccdd;s->eflags=0xad7;
    uint32_t frame[]={0x01001234,a,b};memcpy((void *)(uintptr_t)s->gpr[4],frame,12);
    PwX86State before=*s;unsigned calls=r->calls;
    assert(pw_win32_dispatch(r,s)==expected);
    if(expected==PW_OK) {
        assert(s->gpr[4]==before.gpr[4]+4 && s->eip==frame[0] && s->eflags==0xad7 && r->calls==calls+1);
        for(unsigned i=1;i<8;i++)if(i!=4)assert(s->gpr[i]==before.gpr[i]);
        if(!strcmp(name,"free") || !strcmp(name,"??3@YAXPAX@Z"))
            assert(s->gpr[0]==0xaabbccdd);
    } else assert(!memcmp(s,&before,sizeof(before)) && r->calls==calls);
    return s->gpr[0];
}
static uint32_t kernel_heap_call(PwWin32 *r,PwX86State *s,const char *name,
                                 uint32_t first,uint32_t second,unsigned arguments)
{
    PeImportSymbol symbol={0};PwImportTarget target;strcpy(symbol.name,name);
    assert(pw_win32_resolve(r,"kernel32.dll",&symbol,&target)==PW_OK);
    s->eip=(uint32_t)target.address;s->gpr[4]=s->stack_high-(arguments+1)*4;s->eflags=0xad7;
    uint32_t frame[]={0x01001234,first,second};
    memcpy((void *)(uintptr_t)s->gpr[4],frame,(arguments+1)*4);
    assert(pw_win32_dispatch(r,s)==PW_OK && s->eip==frame[0] && s->gpr[4]==s->stack_high && s->eflags==0xad7);
    return s->gpr[0];
}
static void heap_tests(PwWin32 *r,PwX86State *s,PwVmBackend *vm)
{
    PwVmRegion arena;PwGuestHeap heap;PwHeapBlock blocks[32];
    assert(vm->reserve_at(NULL,0x03400000,65536,4096,&arena)==PW_OK);
    assert(vm->commit(NULL,&arena,0,65536,PW_PROT_READ|PW_PROT_WRITE)==PW_OK);
    assert(pw_guest_heap_init(&heap,0x03400000,65536,blocks,32)==PW_OK);
    r->heap=&heap;r->crt_errno=77;
    s->memory_count=1;s->memory[0]=(PwX86Memory){0x03400000,0x03410000,PW_X86_READ|PW_X86_WRITE};
    uint32_t local=kernel_heap_call(r,s,"LocalAlloc",0x40,32,2);assert(local);
    for(unsigned i=0;i<32;i++)assert(!*(uint8_t *)(uintptr_t)(local+i));
    assert(kernel_heap_call(r,s,"GlobalHandle",local,0,1)==local);
    assert(kernel_heap_call(r,s,"GlobalLock",local,0,1)==local);
    assert(!kernel_heap_call(r,s,"GlobalUnlock",local,0,1));
    assert(!kernel_heap_call(r,s,"LocalFree",local,0,1));
    uint32_t global=kernel_heap_call(r,s,"GlobalAlloc",0x2000,48,2);assert(global);
    assert(!kernel_heap_call(r,s,"GlobalFree",global,0,1));
    uint32_t a=heap_call(r,s,"malloc",36,0,PW_OK);assert(a==0x03400000 && r->crt_errno==77);
    memset((void *)(uintptr_t)a,0x5a,36);
    uint32_t b=heap_call(r,s,"calloc",9,4,PW_OK);assert(b && b!=a);
    for(unsigned i=0;i<36;i++)assert(*(uint8_t *)(uintptr_t)(b+i)==0);
    uint32_t c=heap_call(r,s,"realloc",a,128,PW_OK);assert(c && c!=a);
    for(unsigned i=0;i<36;i++)assert(*(uint8_t *)(uintptr_t)(c+i)==0x5a);
    heap_call(r,s,"free",c+4,0,PW_ERR_PRECONDITION);assert(r->crt_errno==77);
    heap_call(r,s,"??3@YAXPAX@Z",b,0,PW_OK);heap_call(r,s,"free",0,0,PW_OK);
    for(unsigned mode=0;mode<2;mode++) {
        r->new_mode=mode;r->crt_errno=77;
        assert(heap_call(r,s,"realloc",c,UINT32_MAX,PW_OK)==0 && r->crt_errno==12);
        assert(*(uint8_t *)(uintptr_t)c==0x5a);
        r->crt_errno=77;assert(heap_call(r,s,"calloc",UINT32_MAX,2,PW_OK)==0 && r->crt_errno==12);
        r->crt_errno=77;assert(heap_call(r,s,"malloc",65536,0,PW_OK)==0 && r->crt_errno==12);
    }
    assert(heap_call(r,s,"realloc",c,0,PW_OK)==0 && heap.count==1);
    a=heap_call(r,s,"realloc",0,0,PW_OK);assert(a);heap_call(r,s,"free",a,0,PW_OK);
    s->memory[0].permissions=PW_X86_READ;heap_call(r,s,"malloc",1,0,PW_ERR_VM);
    assert(heap.count==1 && !blocks[0].used);
    r->heap=NULL;r->new_mode=0;s->memory_count=0;
    assert(vm->release(NULL,&arena)==PW_OK);
}
static void length_tests(PwWin32 *r,PwX86State *s,PwVmBackend *vm)
{
    PeImportSymbol symbol={0};PwImportTarget target;strcpy(symbol.name,"lstrlenA");
    assert(pw_win32_resolve(r,"kernel32.dll",&symbol,&target)==PW_OK);
    PwVmRegion text;
    assert(vm->reserve_at(NULL,0x03400000,0x100000,4096,&text)==PW_OK);
    assert(vm->commit(NULL,&text,0,0x100000,PW_PROT_READ|PW_PROT_WRITE)==PW_OK);
    s->memory_count=2;
    s->memory[0]=(PwX86Memory){0x03400000,0x03400002,PW_X86_READ};
    s->memory[1]=(PwX86Memory){0x03400002,0x03500000,PW_X86_READ};
    uint8_t *p=text.write_base;
    for(unsigned test=0;test<8;test++) {
        memset(p,'A',0x100000);uint32_t address=0x03400000,expected=0;
        int result=PW_OK;
        switch(test) {
        case 0:address=0;break;
        case 1:p[0]=0;break;
        case 2:p[0]=0xe9;p[1]=0x80;p[3]=0;expected=3;break;
        case 3:p[0xfffff]=0;expected=0xfffff;break;
        case 4:result=PW_ERR_LIMIT;break;
        case 5:address=0x034fffff;result=PW_ERR_VM;break;
        case 6:s->memory[0].permissions=PW_X86_WRITE;result=PW_ERR_VM;break;
        default:address=0xffffffff;result=PW_ERR_VM;break;
        }
        s->eip=(uint32_t)target.address;s->gpr[4]=s->stack_high-8;s->eflags=0xad7;
        uint32_t frame[]={0x01001234,address};memcpy((void *)(uintptr_t)s->gpr[4],frame,8);
        PwX86State before=*s;unsigned calls=r->calls;
        assert(pw_win32_dispatch(r,s)==result);
        if(result==PW_OK) {
            assert(s->gpr[0]==expected && s->gpr[4]==s->stack_high && s->eip==frame[0]);
            assert(s->eflags==before.eflags && r->calls==calls+1);
            for(unsigned reg=1;reg<8;reg++)if(reg!=4)assert(s->gpr[reg]==before.gpr[reg]);
        } else assert(!memcmp(s,&before,sizeof(before)) && r->calls==calls);
        s->memory[0].permissions=PW_X86_READ;
    }
    assert(vm->release(NULL,&text)==PW_OK);s->memory_count=0;
}
static int copy_call(PwWin32 *r,PwX86State *s,const char *name,uint32_t dst,
                     uint32_t src,uint32_t count,unsigned arguments)
{
    PeImportSymbol symbol={0};PwImportTarget target;strcpy(symbol.name,name);
    assert(pw_win32_resolve(r,"kernel32.dll",&symbol,&target)==PW_OK);
    s->eip=(uint32_t)target.address;s->gpr[4]=s->stack_high-4-arguments*4;
    s->gpr[0]=0xaabbccdd;s->gpr[3]=0x33333333;s->gpr[5]=0x55555555;
    s->gpr[6]=0x66666666;s->gpr[7]=0x77777777;s->eflags=0xad7;
    uint32_t frame[]={0x01001234,dst,src,count};
    memcpy((void *)(uintptr_t)s->gpr[4],frame,4+arguments*4);
    return pw_win32_dispatch(r,s);
}
static void copy_tests(PwWin32 *r,PwX86State *s,PwVmBackend *vm)
{
    PeImportSymbol move_symbol={0};PwImportTarget move_target;strcpy(move_symbol.name,"memmove");
    assert(pw_win32_resolve(r,"msvcrt.dll",&move_symbol,&move_target)==PW_OK);
    uint32_t move_base=s->stack_low+700;memcpy((void *)(uintptr_t)move_base,"abcdef",7);
    s->eip=(uint32_t)move_target.address;s->gpr[4]=s->stack_high-16;s->eflags=0xad7;
    uint32_t move_frame[]={0x01001234,move_base+1,move_base,6};
    memcpy((void *)(uintptr_t)s->gpr[4],move_frame,sizeof(move_frame));
    assert(pw_win32_dispatch(r,s)==PW_OK && s->gpr[0]==move_base+1 &&
           !memcmp((void *)(uintptr_t)move_base,"aabcdef",7) && s->gpr[4]==s->stack_high-12);
    PwVmRegion text;
    assert(vm->reserve_at(NULL,0x03400000,8192,4096,&text)==PW_OK);
    assert(vm->commit(NULL,&text,0,8192,PW_PROT_READ|PW_PROT_WRITE)==PW_OK);
    s->memory_count=1;
    s->memory[0]=(PwX86Memory){0x03400000,0x03402000,PW_X86_READ|PW_X86_WRITE};
    uint8_t *p=text.write_base;uint32_t src=0x03400100,dst=0x03400300;
    unsigned calls=r->calls;r->last_error=51;r->crt_errno=77;
    memcpy(p+0x100,"abc",4);memset(p+0x300,0xcc,16);
    assert(copy_call(r,s,"lstrcpyA",dst,src,0,2)==PW_OK);
    assert(s->gpr[0]==dst && s->gpr[4]==s->stack_high && s->eip==0x01001234);
    assert(s->eflags==0xad7 && s->gpr[3]==0x33333333 && s->gpr[5]==0x55555555 &&
           s->gpr[6]==0x66666666 && s->gpr[7]==0x77777777);
    assert(!memcmp(p+0x300,"abc",4) && p[0x304]==0xcc);
    assert(r->last_error==51 && r->crt_errno==77 && r->calls==calls+1);

    memcpy(p+0x100,"abcdef",7);
    assert(copy_call(r,s,"lstrcpyA",src+2,src,0,2)==PW_OK);
    assert(!memcmp(p+0x102,"abcdef",7));
    memset(p+0x300,0xcc,8);calls=r->calls;
    assert(copy_call(r,s,"lstrcpyA",dst,0xffffffffu,0,2)==PW_OK);
    assert(s->gpr[0]==0 && r->last_error==87 && r->calls==calls+1);
    for(unsigned i=0;i<8;i++)assert(p[0x300+i]==0xcc);

    PeImportSymbol symbol={0};PwImportTarget target;strcpy(symbol.name,"GetLastError");
    assert(pw_win32_resolve(r,"kernel32.dll",&symbol,&target)==PW_OK);
    s->eip=(uint32_t)target.address;s->gpr[4]=s->stack_high-4;s->gpr[0]=0;
    uint32_t return_pc=0x01001234;memcpy((void *)(uintptr_t)s->gpr[4],&return_pc,4);
    assert(pw_win32_dispatch(r,s)==PW_OK && s->gpr[0]==87 &&
           s->gpr[4]==s->stack_high && s->eip==return_pc);

    memset(p+0x300,0xcc,16);
    assert(copy_call(r,s,"lstrcpynA",0xffffffffu,0xffffffffu,0,3)==PW_OK);
    assert(s->gpr[0]==0xffffffffu); /* count zero touches neither pointer */
    assert(copy_call(r,s,"lstrcpynA",dst,0xffffffffu,1,3)==PW_OK);
    assert(s->gpr[0]==dst && p[0x300]==0 && p[0x301]==0xcc);
    memcpy(p+0x100,"abcdef",7);
    assert(copy_call(r,s,"lstrcpynA",dst,src,4,3)==PW_OK);
    assert(!memcmp(p+0x300,"abc",4) && p[0x304]==0xcc);
    memcpy(p+0x100,"hi",3);memset(p+0x300,0xcc,8);
    assert(copy_call(r,s,"lstrcpynA",dst,src,UINT32_MAX,3)==PW_OK);
    assert(!memcmp(p+0x300,"hi",3) && p[0x303]==0xcc);
    memcpy(p+0x100,"abcdef",7);
    assert(copy_call(r,s,"lstrcpynA",src,src,4,3)==PW_OK);
    assert(!memcmp(p+0x100,"abc",4));
    calls=r->calls;
    assert(copy_call(r,s,"lstrcpynA",src+1,src,4,3)==PW_ERR_UNSUPPORTED);
    assert(r->calls==calls && !memcmp(p+0x100,"abc",4));

    memcpy(p+0x100,"cd",3);memcpy(p+0x300,"ab",3);p[0x303]=0xcc;
    assert(copy_call(r,s,"lstrcatA",dst,src,0,2)==PW_OK);
    assert(s->gpr[0]==dst && !memcmp(p+0x300,"abcd",5) && p[0x305]==0xcc);
    memcpy(p+0x100,"same",5);calls=r->calls;
    assert(copy_call(r,s,"lstrcatA",src,src,0,2)==PW_ERR_UNSUPPORTED);
    assert(r->calls==calls && !memcmp(p+0x100,"same",5));
    assert(r->crt_errno==77);
    assert(vm->release(NULL,&text)==PW_OK);s->memory_count=0;
}
static void search_tests(PwWin32 *r,PwX86State *s,PwVmBackend *vm)
{
    PwVmRegion text;
    assert(vm->reserve_at(NULL,0x03400000,4096,4096,&text)==PW_OK);
    assert(vm->commit(NULL,&text,0,4096,PW_PROT_READ|PW_PROT_WRITE)==PW_OK);
    s->memory_count=1;s->memory[0]=(PwX86Memory){0x03400000,0x03401000,PW_X86_READ};
    strcpy((char *)(uintptr_t)0x03400000,"pinball -quick demo");
    strcpy((char *)(uintptr_t)0x03400100,"-quick");
    strcpy((char *)(uintptr_t)0x03400200,"pinball -quick demo");
    const char *names[]={"strstr","strstr","lstrcmpA","lstrcmpA"};
    const char *dlls[]={"msvcrt.dll","msvcrt.dll","kernel32.dll","kernel32.dll"};
    uint32_t right[]={0x03400100,0x03400300,0x03400200,0x03400100};
    strcpy((char *)(uintptr_t)0x03400300,"missing");
    uint32_t expected[]={0x03400008,0,0,1};
    for(unsigned i=0;i<4;i++) {
        PeImportSymbol symbol={0};PwImportTarget target;strcpy(symbol.name,names[i]);
        assert(pw_win32_resolve(r,dlls[i],&symbol,&target)==PW_OK);
        s->gpr[4]=s->stack_high-12;s->eip=(uint32_t)target.address;s->eflags=0xad7;
        uint32_t frame[]={0x01001234,0x03400000,right[i]};
        memcpy((void *)(uintptr_t)s->gpr[4],frame,sizeof(frame));
        assert(pw_win32_dispatch(r,s)==PW_OK && s->gpr[0]==expected[i] &&
               s->eip==frame[0] && s->eflags==0xad7);
        assert(s->gpr[4]==s->stack_high-(i<2?8:0));
    }
    PeImportSymbol symbol={0};PwImportTarget target;strcpy(symbol.name,"_strnicmp");
    assert(pw_win32_resolve(r,"msvcrt.dll",&symbol,&target)==PW_OK);
    strcpy((char *)(uintptr_t)0x03400400,"Right Shift");
    strcpy((char *)(uintptr_t)0x03400500,"right shift");
    const uint32_t counts[]={5,11,4,0};const uint32_t expected_case[]={0,0,0,0};
    for(unsigned i=0;i<4;i++) {
        s->gpr[4]=s->stack_high-16;s->eip=(uint32_t)target.address;s->eflags=0xad7;
        uint32_t frame[]={0x01001234,0x03400400,0x03400500,counts[i]};
        memcpy((void *)(uintptr_t)s->gpr[4],frame,sizeof(frame));
        assert(pw_win32_dispatch(r,s)==PW_OK && s->gpr[0]==expected_case[i] &&
               s->gpr[4]==s->stack_high-12 && s->eip==frame[0] && s->eflags==0xad7);
    }
    *(char *)(uintptr_t)0x03400504='x';
    s->gpr[4]=s->stack_high-16;s->eip=(uint32_t)target.address;
    uint32_t mismatch[]={0x01001234,0x03400400,0x03400500,5};
    memcpy((void *)(uintptr_t)s->gpr[4],mismatch,sizeof(mismatch));
    assert(pw_win32_dispatch(r,s)==PW_OK && s->gpr[0]==UINT32_MAX);
    assert(vm->release(NULL,&text)==PW_OK);s->memory_count=0;
}
static void ctype_tests(PwWin32 *r,PwX86State *s)
{
    const char *names[]={"isalnum","isdigit","isspace"};
    const uint32_t values[]={'A','7',' ','!'};
    const uint8_t expected[][4]={{1,1,0,0},{0,1,0,0},{0,0,1,0}};
    for(unsigned api=0;api<3;api++)for(unsigned i=0;i<4;i++) {
        PeImportSymbol symbol={0};PwImportTarget target;strcpy(symbol.name,names[api]);
        assert(pw_win32_resolve(r,"msvcrt.dll",&symbol,&target)==PW_OK);
        s->eip=(uint32_t)target.address;s->gpr[4]=s->stack_high-8;s->eflags=0xad7;
        uint32_t frame[]={0x01001234,values[i]};memcpy((void *)(uintptr_t)s->gpr[4],frame,8);
        assert(pw_win32_dispatch(r,s)==PW_OK && s->gpr[0]==expected[api][i] &&
               s->gpr[4]==s->stack_high-4 && s->eip==frame[0] && s->eflags==0xad7);
    }
    uint32_t text=s->stack_low+720;strcpy((char *)(uintptr_t)text,"  -123px");
    PeImportSymbol symbol={0};PwImportTarget target;strcpy(symbol.name,"atoi");
    assert(pw_win32_resolve(r,"msvcrt.dll",&symbol,&target)==PW_OK);
    s->eip=(uint32_t)target.address;s->gpr[4]=s->stack_high-8;s->eflags=0xad7;
    uint32_t frame[]={0x01001234,text};memcpy((void *)(uintptr_t)s->gpr[4],frame,8);
    assert(pw_win32_dispatch(r,s)==PW_OK && (int32_t)s->gpr[0]==-123 &&
           s->gpr[4]==s->stack_high-4 && s->eip==frame[0] && s->eflags==0xad7);
}
static void user32_tests(PwWin32 *r,PwX86State *s)
{
    uint32_t name=s->stack_low+256,title=s->stack_low+320;
    strcpy((char *)(uintptr_t)name,"PinballUniqueMessage");
    strcpy((char *)(uintptr_t)title,"3D Pinball");
    PeImportSymbol symbol={0};PwImportTarget target;
    strcpy(symbol.name,"RegisterWindowMessageA");
    assert(pw_win32_resolve(r,"user32.dll",&symbol,&target)==PW_OK);
    s->gpr[4]=s->stack_high-8;s->eip=(uint32_t)target.address;s->eflags=0xad7;
    uint32_t message_frame[]={0x01001234,name};
    memcpy((void *)(uintptr_t)s->gpr[4],message_frame,sizeof(message_frame));
    assert(pw_win32_dispatch(r,s)==PW_OK && s->gpr[0]==0xc000 &&
           s->gpr[4]==s->stack_high && s->eip==message_frame[0] && s->eflags==0xad7);
    strcpy(symbol.name,"FindWindowA");
    assert(pw_win32_resolve(r,"user32.dll",&symbol,&target)==PW_OK);
    s->gpr[4]=s->stack_high-12;s->eip=(uint32_t)target.address;
    uint32_t find_frame[]={0x01001234,name,0};
    memcpy((void *)(uintptr_t)s->gpr[4],find_frame,sizeof(find_frame));
    assert(pw_win32_dispatch(r,s)==PW_OK && !s->gpr[0] && s->gpr[4]==s->stack_high);
    r->user32->windows[0]=(PwUser32Window){.handle=0x10001,.used=1};
    strcpy(r->user32->windows[0].class_name,"PinballUniqueMessage");
    strcpy(r->user32->windows[0].title,"3D Pinball");
    find_frame[2]=title;s->gpr[4]=s->stack_high-12;s->eip=(uint32_t)target.address;
    memcpy((void *)(uintptr_t)s->gpr[4],find_frame,sizeof(find_frame));
    assert(pw_win32_dispatch(r,s)==PW_OK && s->gpr[0]==0x10001);
    r->user32->windows[0].wndproc=0x01002000;
    strcpy(symbol.name,"PostMessageA");
    assert(pw_win32_resolve(r,"user32.dll",&symbol,&target)==PW_OK);
    uint32_t post_frame[]={0x01001234,0x10001,0x400,0x55,0x03400100};
    s->gpr[4]=s->stack_high-sizeof(post_frame);s->eip=(uint32_t)target.address;s->eflags=0xad7;
    memcpy((void *)(uintptr_t)s->gpr[4],post_frame,sizeof(post_frame));
    assert(pw_win32_dispatch(r,s)==PW_OK && s->gpr[0]==1 &&
           s->gpr[4]==s->stack_high && s->eip==post_frame[0] && s->eflags==0xad7);
    assert(r->user32->queue_count==1 && r->user32->queue[0].window==0x10001 &&
           r->user32->queue[0].message==0x400 && r->user32->queue[0].wparam==0x55 &&
           r->user32->queue[0].lparam==0x03400100);
    r->user32->queue_count=0;
    strcpy(symbol.name,"PeekMessageA");
    assert(pw_win32_resolve(r,"user32.dll",&symbol,&target)==PW_OK);
    uint32_t message_output=s->stack_low+384;
    uint32_t peek_frame[]={0x01001234,message_output,0x10001,0,0,1};
    s->gpr[4]=s->stack_high-sizeof(peek_frame);s->eip=(uint32_t)target.address;
    memcpy((void *)(uintptr_t)s->gpr[4],peek_frame,sizeof(peek_frame));
    assert(pw_win32_dispatch(r,s)==PW_OK && !s->gpr[0] && r->idle_hint);

    r->services.message_wait=null_message_fixture;
    strcpy(symbol.name,"GetMessageA");
    assert(pw_win32_resolve(r,"user32.dll",&symbol,&target)==PW_OK);
    memset((void *)(uintptr_t)message_output,0xcc,sizeof(PwUser32QueueEntry));
    uint32_t get_frame[]={0x01001234,message_output,0x10001,0,0};
    s->gpr[4]=s->stack_high-20;s->eip=(uint32_t)target.address;
    memcpy((void *)(uintptr_t)s->gpr[4],get_frame,sizeof(get_frame));
    assert(pw_win32_dispatch(r,s)==PW_OK && s->gpr[0]==1 &&
           s->gpr[4]==s->stack_high && s->eip==get_frame[0] && !r->idle_hint);
    PwUser32QueueEntry received;memcpy(&received,(void *)(uintptr_t)message_output,sizeof(received));
    assert(received.window==0x10001 && received.message==0);
    r->services.message_wait=NULL;

    strcpy(symbol.name,"PostQuitMessage");
    assert(pw_win32_resolve(r,"user32.dll",&symbol,&target)==PW_OK);
    uint32_t quit_frame[]={0x01001234,73};
    s->gpr[4]=s->stack_high-sizeof(quit_frame);s->eip=(uint32_t)target.address;
    memcpy((void *)(uintptr_t)s->gpr[4],quit_frame,sizeof(quit_frame));
    assert(pw_win32_dispatch(r,s)==PW_OK && r->user32->quit_pending &&
           r->user32->quit_code==73 && s->gpr[4]==s->stack_high);
    strcpy(symbol.name,"GetMessageA");
    assert(pw_win32_resolve(r,"user32.dll",&symbol,&target)==PW_OK);
    s->gpr[4]=s->stack_high-sizeof(get_frame);s->eip=(uint32_t)target.address;
    memcpy((void *)(uintptr_t)s->gpr[4],get_frame,sizeof(get_frame));
    assert(pw_win32_dispatch(r,s)==PW_OK && !s->gpr[0] && !r->user32->quit_pending);
    memcpy(&received,(void *)(uintptr_t)message_output,sizeof(received));
    assert(received.message==0x12 && received.wparam==73 && !received.window);
    r->user32->windows[0]=(PwUser32Window){0};

    strcpy(symbol.name,"MapVirtualKeyA");
    assert(pw_win32_resolve(r,"user32.dll",&symbol,&target)==PW_OK);
    const uint32_t scans[]={0x29,0x2a,0x2b,0x36,0x37};
    const uint32_t keys[]={0xc0,0x10,0xdc,0x10,0x6a};
    for(unsigned i=0;i<sizeof(scans)/sizeof(scans[0]);i++) {
        s->gpr[4]=s->stack_high-12;s->eip=(uint32_t)target.address;s->eflags=0xad7;
        uint32_t frame[]={0x01001234,scans[i],1};
        memcpy((void *)(uintptr_t)s->gpr[4],frame,sizeof(frame));
        assert(pw_win32_dispatch(r,s)==PW_OK && s->gpr[0]==keys[i] &&
               s->gpr[4]==s->stack_high && s->eip==frame[0] && s->eflags==0xad7);
    }
    strcpy(symbol.name,"GetKeyNameTextA");
    assert(pw_win32_resolve(r,"user32.dll",&symbol,&target)==PW_OK);
    uint32_t output=s->stack_low+384;
    memset((void *)(uintptr_t)output,0xcc,20);
    s->gpr[4]=s->stack_high-16;s->eip=(uint32_t)target.address;s->eflags=0xad7;
    uint32_t key_frame[]={0x01001234,0x00360000,output,19};
    memcpy((void *)(uintptr_t)s->gpr[4],key_frame,sizeof(key_frame));
    assert(pw_win32_dispatch(r,s)==PW_OK && s->gpr[0]==11 &&
           !strcmp((char *)(uintptr_t)output,"Right Shift") &&
           s->gpr[4]==s->stack_high && s->eip==key_frame[0] && s->eflags==0xad7);
    s->gpr[4]=s->stack_high-16;s->eip=(uint32_t)target.address;
    key_frame[2]=s->stack_high-2;memcpy((void *)(uintptr_t)s->gpr[4],key_frame,sizeof(key_frame));
    PwX86State before=*s;unsigned calls=r->calls;
    assert(pw_win32_dispatch(r,s)==PW_ERR_VM && !memcmp(s,&before,sizeof(before)) &&
           r->calls==calls);
}
static int registry_call(PwWin32 *r,PwX86State *s,const char *name,
                         const uint32_t *args,unsigned count)
{
    PeImportSymbol symbol={0};PwImportTarget target;strcpy(symbol.name,name);
    assert(pw_win32_resolve(r,"advapi32.dll",&symbol,&target)==PW_OK);
    s->eip=(uint32_t)target.address;s->gpr[4]=s->stack_high-4-count*4;s->eflags=0xad7;
    uint32_t frame[10]={0x01001234};memcpy(frame+1,args,count*4);
    memcpy((void *)(uintptr_t)s->gpr[4],frame,(count+1)*4);
    return pw_win32_dispatch(r,s);
}
static void registry_tests(PwWin32 *r,PwX86State *s)
{
    uint32_t base=s->stack_low+0x100,key_name=base,value_name=base+64;
    uint32_t key_out=base+128,disposition=base+132,size_out=base+136;
    strcpy((char *)(uintptr_t)key_name,"Software\\Pinball");
    strcpy((char *)(uintptr_t)value_name,"Table Version");
    uint32_t create[]={PW_HKEY_CURRENT_USER,key_name,0,0,0,0xf003f,0,key_out,disposition};
    unsigned calls=r->calls;
    assert(registry_call(r,s,"RegCreateKeyExA",create,9)==PW_OK && s->gpr[0]==0);
    assert(s->eip==0x01001234 && s->gpr[4]==s->stack_high && s->eflags==0xad7);
    uint32_t key;memcpy(&key,(void *)(uintptr_t)key_out,4);
    uint32_t disp;memcpy(&disp,(void *)(uintptr_t)disposition,4);
    assert(key && disp==PW_REG_CREATED_NEW_KEY && r->calls==calls+1);

    uint32_t default_value=7;memcpy((void *)(uintptr_t)(base+144),&default_value,4);
    uint32_t set[]={key,value_name,0,PW_REG_DWORD,base+144,4};
    assert(registry_call(r,s,"RegSetValueExA",set,6)==PW_OK && s->gpr[0]==0);
    uint32_t capacity=4;memcpy((void *)(uintptr_t)size_out,&capacity,4);
    uint32_t query[]={key,value_name,0,base+140,base+148,size_out};
    assert(registry_call(r,s,"RegQueryValueExA",query,6)==PW_OK && s->gpr[0]==0);
    uint32_t output,type;memcpy(&output,(void *)(uintptr_t)(base+148),4);
    memcpy(&type,(void *)(uintptr_t)(base+140),4);memcpy(&capacity,(void *)(uintptr_t)size_out,4);
    assert(output==7 && type==PW_REG_DWORD && capacity==4);

    capacity=2;memcpy((void *)(uintptr_t)size_out,&capacity,4);
    output=0xfeedbeef;memcpy((void *)(uintptr_t)(base+148),&output,4);
    assert(registry_call(r,s,"RegQueryValueExA",query,6)==PW_OK && s->gpr[0]==PW_REG_ERROR_MORE_DATA);
    memcpy(&output,(void *)(uintptr_t)(base+148),4);memcpy(&capacity,(void *)(uintptr_t)size_out,4);
    assert(output==0xfeedbeef && capacity==4);

    uint32_t close[]={key};assert(registry_call(r,s,"RegCloseKey",close,1)==PW_OK && s->gpr[0]==0);
    uint32_t open[]={PW_HKEY_CURRENT_USER,key_name,key_out};
    assert(registry_call(r,s,"RegOpenKeyA",open,3)==PW_OK && s->gpr[0]==0);
    memcpy(&key,(void *)(uintptr_t)key_out,4);
    strcpy((char *)(uintptr_t)(base+160),"default");
    uint32_t set_default[]={key,0,0,PW_REG_SZ,base+160,8};
    assert(registry_call(r,s,"RegSetValueExA",set_default,6)==PW_OK && s->gpr[0]==0);
    capacity=16;memcpy((void *)(uintptr_t)size_out,&capacity,4);
    uint32_t query_default[]={key,0,base+176,size_out};
    assert(registry_call(r,s,"RegQueryValueA",query_default,4)==PW_OK && s->gpr[0]==0);
    assert(!strcmp((char *)(uintptr_t)(base+176),"default"));
    memcpy(&capacity,(void *)(uintptr_t)size_out,4);assert(capacity==8);
    close[0]=key;assert(registry_call(r,s,"RegCloseKey",close,1)==PW_OK && s->gpr[0]==0);
    uint32_t open_ex[]={PW_HKEY_CURRENT_USER,key_name,0,0x20019,key_out};
    assert(registry_call(r,s,"RegOpenKeyExA",open_ex,5)==PW_OK && s->gpr[0]==0);
    memcpy(&key,(void *)(uintptr_t)key_out,4);close[0]=key;
    assert(registry_call(r,s,"RegCloseKey",close,1)==PW_OK && s->gpr[0]==0);
    calls=r->calls;create[7]=0xffffffffu;
    assert(registry_call(r,s,"RegCreateKeyExA",create,9)==PW_ERR_VM);
    assert(r->calls==calls && r->registry->keys[0].open_count==0);
}
static void current_directory_tests(PwWin32 *runtime,PwX86State *state)
{
    PeImportSymbol symbol={0};PwImportTarget target;
    const char *directory="C:\\Games\\Pinball";
    size_t length=strlen(directory);
    uint32_t destination=state->stack_low+128;
    runtime->services.current_directory=directory;
    strcpy(symbol.name,"GetCurrentDirectoryA");
    assert(pw_win32_resolve(runtime,"kernel32.dll",&symbol,&target)==PW_OK);

    runtime->last_error=0x1234;
    state->gpr[4]=state->stack_high-12;state->eip=(uint32_t)target.address;
    uint32_t query[]={0x01001234,0,0};
    memcpy((void *)(uintptr_t)state->gpr[4],query,sizeof(query));
    assert(pw_win32_dispatch(runtime,state)==PW_OK &&
           state->gpr[0]==length+1u && runtime->last_error==0x1234);

    memset((void *)(uintptr_t)destination,0xcc,length+2u);
    state->gpr[4]=state->stack_high-12;state->eip=(uint32_t)target.address;
    uint32_t short_buffer[]={0x01001234,(uint32_t)length,destination};
    memcpy((void *)(uintptr_t)state->gpr[4],short_buffer,sizeof(short_buffer));
    assert(pw_win32_dispatch(runtime,state)==PW_OK &&
           state->gpr[0]==length+1u && runtime->last_error==0x1234 &&
           *(uint8_t *)(uintptr_t)destination==0xcc);

    state->gpr[4]=state->stack_high-12;state->eip=(uint32_t)target.address;
    uint32_t enough[]={0x01001234,(uint32_t)length+1u,destination};
    memcpy((void *)(uintptr_t)state->gpr[4],enough,sizeof(enough));
    assert(pw_win32_dispatch(runtime,state)==PW_OK && state->gpr[0]==length &&
           !strcmp((char *)(uintptr_t)destination,directory));

    state->gpr[4]=state->stack_high-12;state->eip=(uint32_t)target.address;
    uint32_t bad_null[]={0x01001234,(uint32_t)length+1u,0};
    memcpy((void *)(uintptr_t)state->gpr[4],bad_null,sizeof(bad_null));
    assert(pw_win32_dispatch(runtime,state)==PW_OK && state->gpr[0]==0 &&
           runtime->last_error==87);
}

int main(void)
{
    PwVmBackend vm;PwVmRegion data;
    assert(pw_vm_posix_backend(&vm)==PW_OK);
    assert(vm.reserve_at(NULL,0x03000000,8192,4096,&data)==PW_OK);
    assert(vm.commit(NULL,&data,0,8192,PW_PROT_READ|PW_PROT_WRITE)==PW_OK);
    PwWin32 runtime={0};
    assert(pw_win32_init(&runtime,0x01000000,0x03000000,"\"C:\\game\\sample.exe\"")==PW_OK);
    runtime.services.main_module_filename="C:\\game\\sample.exe";
    PwRegistry registry;PwRegistryKey registry_keys[8];PwRegistryValue registry_values[16];
    assert(pw_registry_init(&registry,registry_keys,8,registry_values,16)==PW_OK);
    PwUser32 user32;PwUser32Message user_messages[8];PwUser32Window user_windows[8];
    PwUser32Resource user_resources[8];PwUser32Class user_classes[8];
    assert(pw_user32_init(&user32,user_messages,8,user_windows,8)==PW_OK);
    assert(pw_user32_init_resources(&user32,user_resources,8)==PW_OK);
    assert(pw_user32_init_classes(&user32,user_classes,8)==PW_OK);
    runtime.registry=&registry;runtime.user32=&user32;
    runtime.services.named_resource=named_fixture;
    runtime.services.code_address=code_fixture;
    PeImportSymbol symbol={0};PwImportTarget target;
    strcpy(symbol.name,"_acmdln");
    assert(pw_win32_resolve(&runtime,"MSVCRT.DLL",&symbol,&target)==PW_OK && target.kind==PW_IMPORT_DATA);
    uint32_t pointer;memcpy(&pointer,(void *)(uintptr_t)target.address,4);
    assert(!strcmp((char *)(uintptr_t)pointer,"\"C:\\game\\sample.exe\""));
    strcpy(symbol.name,"_adjust_fdiv");
    assert(pw_win32_resolve(&runtime,"msvcrt.dll",&symbol,&target)==PW_OK && target.kind==PW_IMPORT_DATA);
    uint32_t adjust;memcpy(&adjust,(void *)(uintptr_t)target.address,4);assert(adjust==0);
    strcpy(symbol.name,"GetModuleHandleA");
    assert(pw_win32_resolve(&runtime,"KERNEL32.dll",&symbol,&target)==PW_OK && target.kind==PW_IMPORT_FUNCTION);
    PwX86State state={0};state.stack_low=0x03001000;state.stack_high=0x03002000;
    state.gpr[4]=state.stack_high-8;state.eip=(uint32_t)target.address;
    uint32_t words[]={0x01001234,0};memcpy((void *)(uintptr_t)state.gpr[4],words,8);
    assert(pw_win32_dispatch(&runtime,&state)==PW_OK);
    assert(state.gpr[0]==0x01000000 && state.eip==words[0] && state.gpr[4]==state.stack_high);
    assert(runtime.calls==1);
    strcpy(symbol.name,"GetModuleFileNameA");
    assert(pw_win32_resolve(&runtime,"kernel32.dll",&symbol,&target)==PW_OK);
    for(unsigned test=0;test<4;test++) {
        uint32_t destination=state.stack_low+64,capacity=test==0?64:test==1?4:test==2?0:64;
        uint32_t module=test==3?0x02000000:0;
        memset((void *)(uintptr_t)destination,0xcc,64);
        state.gpr[4]=state.stack_high-16;state.eip=(uint32_t)target.address;state.gpr[0]=0xaabbccdd;
        uint32_t filename_frame[]={0x01001234,module,destination,capacity};
        memcpy((void *)(uintptr_t)state.gpr[4],filename_frame,sizeof(filename_frame));
        PwX86State filename_before=state;unsigned filename_calls=runtime.calls;
        int expected=test==3?PW_ERR_UNSUPPORTED:PW_OK;
        assert(pw_win32_dispatch(&runtime,&state)==expected);
        if(expected==PW_OK) {
            uint32_t length=test==0?18:test==1?4:0;
            assert(state.gpr[0]==length && state.gpr[4]==state.stack_high &&
                   state.eip==filename_frame[0] && runtime.calls==filename_calls+1);
            if(test==0)assert(!strcmp((char *)(uintptr_t)destination,"C:\\game\\sample.exe"));
            else if(test==1)assert(!memcmp((void *)(uintptr_t)destination,"C:\\g",4) &&
                                   *(uint8_t *)(uintptr_t)(destination+4)==0xcc);
            else assert(*(uint8_t *)(uintptr_t)destination==0xcc);
        } else assert(!memcmp(&state,&filename_before,sizeof(state)) && runtime.calls==filename_calls);
    }
    strcpy(symbol.name,"GetModuleHandleA");
    assert(pw_win32_resolve(&runtime,"kernel32.dll",&symbol,&target)==PW_OK);
    state.gpr[4]-=8;state.eip=(uint32_t)target.address;words[1]=0x03000010;
    memcpy((void *)(uintptr_t)state.gpr[4],words,8);PwX86State before=state;
    unsigned calls_before=runtime.calls;
    assert(pw_win32_dispatch(&runtime,&state)==PW_ERR_UNSUPPORTED);
    assert(memcmp(&state,&before,sizeof(state))==0 && runtime.calls==calls_before);
    strcpy(symbol.name,"GetLastError");
    assert(pw_win32_resolve(&runtime,"kernel32.dll",&symbol,&target)==PW_OK);
    state.gpr[4]=state.stack_high-4;state.eip=(uint32_t)target.address;
    memcpy((void *)(uintptr_t)state.gpr[4],words,4);
    assert(pw_win32_dispatch(&runtime,&state)==PW_OK && state.gpr[0]==0 &&
           state.gpr[4]==state.stack_high && state.eip==words[0]);
    resource_tests(&runtime,&state);
    class_tests(&runtime,&state);
    const char *crt_names[]={"__set_app_type","__p__fmode","__p__commode"};
    for(unsigned i=0;i<3;i++) {
        strcpy(symbol.name,crt_names[i]);
        assert(pw_win32_resolve(&runtime,"MSVCRT.DLL",&symbol,&target)==PW_OK);
        state.gpr[4]=state.stack_high-8;state.eip=(uint32_t)target.address;
        state.gpr[0]=0xaabbccdd;state.eflags=0x246;
        words[1]=2;memcpy((void *)(uintptr_t)state.gpr[4],words,8);
        assert(pw_win32_dispatch(&runtime,&state)==PW_OK);
        assert(state.gpr[4]==state.stack_high-4 && state.eip==words[0] && state.eflags==0x246);
        if(!i)assert(runtime.app_type==2 && state.gpr[0]==0xaabbccdd);
        else {
            assert(state.gpr[0]==runtime.crt_data+(i==1?8:12));
            uint32_t value;memcpy(&value,(void *)(uintptr_t)state.gpr[0],4);
            assert(value==(i==1?0x4000u:0));
            value=123;memcpy((void *)(uintptr_t)state.gpr[0],&value,4);
            state.gpr[4]=state.stack_high-8;state.eip=(uint32_t)target.address;
            assert(pw_win32_dispatch(&runtime,&state)==PW_OK);
            memcpy(&value,(void *)(uintptr_t)state.gpr[0],4);assert(value==123);
        }
    }
    strcpy(symbol.name,"__set_app_type");
    assert(pw_win32_resolve(&runtime,"msvcrt.dll",&symbol,&target)==PW_OK);
    state.gpr[4]=state.stack_high-4;state.eip=(uint32_t)target.address;before=state;
    unsigned calls=runtime.calls;
    assert(pw_win32_dispatch(&runtime,&state)==PW_ERR_VM);
    assert(!memcmp(&state,&before,sizeof(state)) && runtime.app_type==2 && runtime.calls==calls);
    strcpy(symbol.name,"_controlfp");
    assert(pw_win32_resolve(&runtime,"msvcrt.dll",&symbol,&target)==PW_OK);
    state.gpr[4]=state.stack_high-12;state.eip=(uint32_t)target.address;
    uint32_t fp_args[]={0x01001234,0x200,0x300};
    memcpy((void *)(uintptr_t)state.gpr[4],fp_args,sizeof(fp_args));before=state;
    assert(pw_win32_dispatch(&runtime,&state)==PW_ERR_STATE && !memcmp(&state,&before,sizeof(state)));
    pw_guest_fp_init(&state.fp);
    assert(pw_win32_dispatch(&runtime,&state)==PW_OK);
    assert(state.gpr[0]==0x9021f && state.gpr[4]==state.stack_high-8 && state.eip==fp_args[0]);
    assert((state.fp.x87_control&0xc00)==0x800 && (state.fp.mxcsr&0x6000)==0x4000);
    state.gpr[4]=state.stack_high-8;state.eip=(uint32_t)target.address;before=state;
    assert(pw_win32_dispatch(&runtime,&state)==PW_ERR_VM && !memcmp(&state,&before,sizeof(state)));
    strcpy(symbol.name,"__getmainargs");
    assert(pw_win32_resolve(&runtime,"msvcrt.dll",&symbol,&target)==PW_OK);
    state.gpr[4]=state.stack_high-64;state.eip=(uint32_t)target.address;
    uint32_t main_args[]={0x01001234,state.stack_high-16,state.stack_high-12,state.stack_high-8,0,0};
    memcpy((void *)(uintptr_t)state.gpr[4],main_args,sizeof(main_args));
    assert(pw_win32_dispatch(&runtime,&state)==PW_OK && state.gpr[0]==0 && state.gpr[4]==state.stack_high-60);
    uint32_t outputs[3];memcpy(outputs,(void *)(uintptr_t)(state.stack_high-16),12);
    assert(outputs[0]==1 && outputs[1]==runtime.args.argv && outputs[2]==runtime.args.envp);
    uint32_t arg0;memcpy(&arg0,(void *)(uintptr_t)outputs[1],4);
    assert(!strcmp((char *)(uintptr_t)arg0,"C:\\game\\sample.exe"));
    for(unsigned failure=0;failure<2;failure++) {
        state.gpr[4]=state.stack_high-64;state.eip=(uint32_t)target.address;
        main_args[3]=failure?state.stack_high:state.stack_high-8;main_args[4]=failure?0:1;
        memcpy((void *)(uintptr_t)state.gpr[4],main_args,sizeof(main_args));before=state;
        assert(pw_win32_dispatch(&runtime,&state)==(failure?PW_ERR_VM:PW_ERR_UNSUPPORTED));
        uint32_t after[3];memcpy(after,(void *)(uintptr_t)(state.stack_high-16),12);
        assert(!memcmp(&state,&before,sizeof(state)) && !memcmp(after,outputs,sizeof(after)));
    }
    strcpy(symbol.name,"GetStartupInfoA");
    assert(pw_win32_resolve(&runtime,"kernel32.dll",&symbol,&target)==PW_OK);
    for(unsigned show=0;show<3;show++) {
        runtime.startup_show=(uint16_t)show;
        uint32_t address=state.stack_low+1; /* unaligned output is supported */
        memset((void *)(uintptr_t)state.stack_low,0xcc,72);
        state.gpr[4]=state.stack_high-8;state.eip=(uint32_t)target.address;state.gpr[0]=0xaabbccdd;
        uint32_t startup_frame[]={0x01001234,address};memcpy((void *)(uintptr_t)state.gpr[4],startup_frame,8);
        assert(pw_win32_dispatch(&runtime,&state)==PW_OK && state.gpr[0]==0xaabbccdd && state.gpr[4]==state.stack_high);
        uint8_t expected[68]={0};expected[0]=68;expected[44]=1;expected[48]=(uint8_t)show;
        assert(!memcmp((void *)(uintptr_t)address,expected,68));
        assert(*(uint8_t *)(uintptr_t)state.stack_low==0xcc && *(uint8_t *)(uintptr_t)(address+68)==0xcc);
    }
    state.gpr[4]=state.stack_high-8;state.eip=(uint32_t)target.address;
    uint32_t startup_bad[]={0x01001234,state.stack_high-67};
    memcpy((void *)(uintptr_t)state.gpr[4],startup_bad,8);before=state;
    uint8_t snapshot[67];memcpy(snapshot,(void *)(uintptr_t)startup_bad[1],sizeof(snapshot));
    assert(pw_win32_dispatch(&runtime,&state)==PW_ERR_VM && !memcmp(&state,&before,sizeof(state)));
    assert(!memcmp(snapshot,(void *)(uintptr_t)startup_bad[1],sizeof(snapshot)));
    runtime.startup_show=10;
    assert(pw_win32_dispatch(&runtime,&state)==PW_ERR_UNSUPPORTED && !memcmp(&state,&before,sizeof(state)));
    strcpy(symbol.name,"NotInCatalog");
    assert(pw_win32_resolve(&runtime,"kernel32.dll",&symbol,&target)==PW_ERR_NOT_FOUND);
    symbol.by_ordinal=1;symbol.ordinal=42;
    assert(pw_win32_resolve(&runtime,"kernel32.dll",&symbol,&target)==PW_ERR_UNSUPPORTED);
    symbol.by_ordinal=0;strcpy(symbol.name,"exit");
    assert(pw_win32_resolve(&runtime,"msvcrt.dll",&symbol,&target)==PW_OK);
    uint32_t exit_frame[]={0x01001234,29};
    state.gpr[4]=state.stack_high-sizeof(exit_frame);state.eip=(uint32_t)target.address;
    memcpy((void *)(uintptr_t)state.gpr[4],exit_frame,sizeof(exit_frame));
    assert(pw_win32_dispatch(&runtime,&state)==PW_OK && runtime.exit_requested &&
           runtime.exit_code==29 && state.eip==exit_frame[0] &&
           state.gpr[4]==state.stack_high-4); /* cdecl leaves the argument */
    runtime.exit_requested=0;runtime.exit_code=0;
    string_tests(&runtime,&state);
    current_directory_tests(&runtime,&state);
    itoa_tests(&runtime,&state);
    profile_tests(&runtime,&state);
    waveout_tests(&runtime,&state);
    length_tests(&runtime,&state,&vm);
    copy_tests(&runtime,&state,&vm);
    search_tests(&runtime,&state,&vm);
    ctype_tests(&runtime,&state);
    user32_tests(&runtime,&state);
    registry_tests(&runtime,&state);
    heap_tests(&runtime,&state,&vm);
    assert(vm.release(NULL,&data)==PW_OK);return 0;
}
