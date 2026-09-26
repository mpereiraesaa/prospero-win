/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_win32.h"
#include "pw_crt_format.h"
#include "pw_module_name.h"
#include "pw_x87.h"
#include <string.h>
#include "pw_win32_catalog.h"
static uint16_t load_le16(const uint8_t *p){return (uint16_t)p[0]|(uint16_t)p[1]<<8;}
static uint32_t load_le32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t floor_binary64(uint64_t bits)
{
    uint64_t magnitude=bits&UINT64_C(0x7fffffffffffffff),sign=bits>>63;
    unsigned exponent=(unsigned)(magnitude>>52);
    if(exponent==0x7ff || !magnitude)return bits;
    int unbiased=(int)exponent-1023;
    if(unbiased>=52)return bits;
    if(unbiased<0)return sign?UINT64_C(0xbff0000000000000):0;
    uint64_t mask=(UINT64_C(1)<<(52-unbiased))-1;
    if(!(magnitude&mask))return bits;
    magnitude&=~mask;if(sign)magnitude+=UINT64_C(1)<<(52-unbiased);
    return magnitude|(sign<<63);
}
int pw_win32_init(PwWin32 *runtime,uint32_t main,uint32_t data,const char *line)
{
    if(!runtime || !main || !data || !line || (uint64_t)data+4096>UINT32_MAX)
        return PW_ERR_PRECONDITION;
    size_t length=strlen(line);
    if(length>4096-17)return PW_ERR_LIMIT;
    uint8_t buffer[4096]={0};PwGuestArgs args;
    size_t offset=(16+length+1+3)&~(size_t)3;
    if(offset>=sizeof(buffer))return PW_ERR_LIMIT;
    int status=pw_guest_args_build(line,NULL,0,data+(uint32_t)offset,buffer+offset,sizeof(buffer)-offset,&args);
    if(status!=PW_OK)return status;
    uint32_t pointer=data+16,text_mode=0x4000;
    memcpy(buffer,&pointer,4);memcpy(buffer+8,&text_mode,4);
    memcpy(buffer+16,line,length+1);
    memcpy((void *)(uintptr_t)data,buffer,sizeof(buffer));
    *runtime=(PwWin32){.main_base=main,.crt_data=data,.args=args,.startup_show=1,
        .rand_state=1};return PW_OK;
}
int pw_win32_resolve(void *opaque,const char *dll,const PeImportSymbol *symbol,PwImportTarget *target)
{
    PwWin32 *r=opaque;
    if(!r || !dll || !symbol || !target)return PW_ERR_PRECONDITION;
    if(!r->main_base || !r->crt_data)return PW_ERR_STATE;
    if(symbol->by_ordinal)return PW_ERR_UNSUPPORTED;
    for(unsigned i=0;i<sizeof(pw_catalog)/sizeof(pw_catalog[0]);i++) {
        if(!pw_module_name_equal(dll,pw_catalog[i].dll) || strcmp(symbol->name,pw_catalog[i].name))continue;
        uint32_t address=PW_WIN32_TOKEN_BASE+i*16;
        if(pw_catalog[i].kind==PW_IMPORT_DATA) {
            if(!strcmp(pw_catalog[i].dll,"msvcrt.dll") && !strcmp(symbol->name,"_acmdln"))address=r->crt_data;
            else if(!strcmp(pw_catalog[i].dll,"msvcrt.dll") && !strcmp(symbol->name,"_adjust_fdiv"))address=r->crt_data+4;
            else return PW_ERR_UNSUPPORTED;
        }
        *target=(PwImportTarget){address,pw_catalog[i].kind};return PW_OK;
    }
    return PW_ERR_NOT_FOUND;
}
static int range_access(PwX86State *s,uint32_t address,size_t bytes,unsigned permission)
{
    if(bytes>UINT32_MAX)return PW_ERR_VM;
    uint64_t end=(uint64_t)address+bytes;
    if(!address || end>0x100000000ull || s->memory_count>PW_X86_MEMORY_REGIONS)return PW_ERR_VM;
    unsigned readable=address>=s->stack_low && end<=s->stack_high;
    for(unsigned i=0;i<s->memory_count;i++) {
        PwX86Memory *m=&s->memory[i];
        if(m->high<=0x100000000ull && address>=m->low && end<=m->high && (m->permissions&permission)==permission)readable=1;
    }
    if(!readable)return PW_ERR_VM;
    return PW_OK;
}
static int word_access(PwX86State *s,uint32_t address,unsigned permission)
{return range_access(s,address,4,permission);}
static int string_length(PwX86State *s,uint32_t address,uint32_t *length)
{
    /* Bounded guest reads only; never hand an unvalidated pointer to strlen. */
    for(uint32_t n=0;n<0x100000;n++) {
        if(address>UINT32_MAX-n)return PW_ERR_VM;
        int status=range_access(s,address+n,1,PW_X86_READ);
        if(status!=PW_OK)return status;
        if(!*(const uint8_t *)(uintptr_t)(address+n)){*length=n;return PW_OK;}
    }
    return PW_ERR_LIMIT;
}
static int guest_string(PwX86State *s,uint32_t address,char *output,size_t capacity,
                        unsigned nullable)
{
    if(!address) {
        if(!nullable)return PW_ERR_VM;
        output[0]=0;return PW_OK;
    }
    uint32_t length;int status=string_length(s,address,&length);
    if(status!=PW_OK)return status;
    if((uint64_t)length+1>capacity)return PW_ERR_LIMIT;
    memcpy(output,(const void *)(uintptr_t)address,(size_t)length+1);return PW_OK;
}
static int writable_capacity(PwX86State *s,uint32_t address,size_t limit,size_t *capacity)
{
    if(!address || !capacity || !limit || s->memory_count>PW_X86_MEMORY_REGIONS)return PW_ERR_VM;
    uint64_t best=0;
    if(address>=s->stack_low && address<s->stack_high)best=s->stack_high-address;
    for(unsigned i=0;i<s->memory_count;i++) {
        const PwX86Memory *m=&s->memory[i];
        if((m->permissions&PW_X86_WRITE) && m->high<=0x100000000ull &&
           address>=m->low && address<m->high && m->high-address>best)best=m->high-address;
    }
    if(!best)return PW_ERR_VM;
    *capacity=(size_t)(best<limit?best:limit);return PW_OK;
}
typedef struct FormatGuest { const PwGuestCall *call;PwX86State *state; } FormatGuest;
static int format_u32(void *opaque,unsigned index,uint32_t *value)
{
    FormatGuest *guest=opaque;
    if(index>(UINT32_MAX-8)/4)return PW_ERR_LIMIT;
    return pw_guest_call_u32(guest->call,8+index*4,value);
}
static int format_string(void *opaque,uint32_t address,char *output,size_t capacity,size_t *length)
{
    FormatGuest *guest=opaque;uint32_t n;int status=string_length(guest->state,address,&n);
    if(status!=PW_OK)return status;
    if((uint64_t)n+1>capacity)return PW_ERR_LIMIT;
    memcpy(output,(const void *)(uintptr_t)address,(size_t)n+1);*length=n;return PW_OK;
}
static int registry_dispatch(PwWin32 *r,PwX86State *state)
{
    if(strcmp(r->last_dll,"advapi32.dll"))return PW_ERR_NOT_FOUND;
    if(!r->registry)return PW_ERR_STATE;
    const char *name=r->last_name;PwGuestCall call={0};uint32_t a[9]={0};
    unsigned count;
    if(!strcmp(name,"RegCreateKeyExA"))count=9;
    else if(!strcmp(name,"RegOpenKeyA"))count=3;
    else if(!strcmp(name,"RegOpenKeyExA"))count=5;
    else if(!strcmp(name,"RegQueryValueA"))count=4;
    else if(!strcmp(name,"RegQueryValueExA") || !strcmp(name,"RegSetValueExA"))count=6;
    else if(!strcmp(name,"RegCloseKey"))count=1;
    else return PW_ERR_UNSUPPORTED;
    int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,count*4,0);
    if(status!=PW_OK)return status;
    for(unsigned i=0;i<count;i++)if((status=pw_guest_call_u32(&call,i*4,&a[i]))!=PW_OK)return status;

    if(!strcmp(name,"RegCloseKey")) {
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,0))!=PW_OK)return status;
        uint32_t result=pw_registry_close(r->registry,a[0]);after.gpr[0]=result;
        *state=after;r->calls++;return PW_OK;
    }
    if(!strcmp(name,"RegCreateKeyExA")) {
        char key[PW_REG_PATH_MAX+1];
        if(a[2] || a[3] || a[4] || a[6])return PW_ERR_UNSUPPORTED;
        if((status=guest_string(state,a[1],key,sizeof(key),0))!=PW_OK)return status;
        if((status=word_access(state,a[7],PW_X86_WRITE))!=PW_OK)return status;
        if(a[8] && (status=word_access(state,a[8],PW_X86_WRITE))!=PW_OK)return status;
        if(a[8] && (uint64_t)a[7]<a[8]+4ull && (uint64_t)a[8]<a[7]+4ull)
            return PW_ERR_UNSUPPORTED;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,0))!=PW_OK)return status;
        uint32_t handle=0,disposition=0;
        uint32_t result=pw_registry_create(r->registry,a[0],key,&handle,&disposition);
        after.gpr[0]=result;
        if(!result){memcpy((void *)(uintptr_t)a[7],&handle,4);if(a[8])memcpy((void *)(uintptr_t)a[8],&disposition,4);}
        *state=after;r->calls++;return PW_OK;
    }
    if(!strcmp(name,"RegOpenKeyA") || !strcmp(name,"RegOpenKeyExA")) {
        unsigned extended=!strcmp(name,"RegOpenKeyExA");uint32_t result_pointer=a[extended?4:2];
        if(extended && a[2])return PW_ERR_UNSUPPORTED;
        char key[PW_REG_PATH_MAX+1];
        if((status=guest_string(state,a[1],key,sizeof(key),1))!=PW_OK)return status;
        if((status=word_access(state,result_pointer,PW_X86_WRITE))!=PW_OK)return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,0))!=PW_OK)return status;
        uint32_t handle=0,result=pw_registry_open(r->registry,a[0],key,&handle);
        after.gpr[0]=result;if(!result)memcpy((void *)(uintptr_t)result_pointer,&handle,4);
        *state=after;r->calls++;return PW_OK;
    }
    if(!strcmp(name,"RegSetValueExA")) {
        if(a[2])return PW_ERR_UNSUPPORTED;
        char value_name[PW_REG_NAME_MAX+1];uint8_t data[PW_REG_DATA_MAX];
        if((status=guest_string(state,a[1],value_name,sizeof(value_name),1))!=PW_OK)return status;
        if(a[5]>sizeof(data))return PW_ERR_UNSUPPORTED;
        if(a[5] && (status=range_access(state,a[4],a[5],PW_X86_READ))!=PW_OK)return status;
        if(a[5])memcpy(data,(const void *)(uintptr_t)a[4],a[5]);
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,0))!=PW_OK)return status;
        uint32_t result=pw_registry_set(r->registry,a[0],value_name,a[3],data,a[5]);
        after.gpr[0]=result;*state=after;r->calls++;return PW_OK;
    }
    if(!strcmp(name,"RegQueryValueExA")) {
        if(a[2])return PW_ERR_UNSUPPORTED;
        char value_name[PW_REG_NAME_MAX+1];uint8_t data[PW_REG_DATA_MAX];uint32_t type=0,size;
        if((status=guest_string(state,a[1],value_name,sizeof(value_name),1))!=PW_OK)return status;
        if((status=word_access(state,a[5],PW_X86_READ|PW_X86_WRITE))!=PW_OK)return status;
        memcpy(&size,(const void *)(uintptr_t)a[5],4);
        if(a[3] && (status=word_access(state,a[3],PW_X86_WRITE))!=PW_OK)return status;
        uint32_t result=pw_registry_query(r->registry,a[0],value_name,&type,
                                          a[4]?data:NULL,&size);
        if(!result && a[4] && size && (status=range_access(state,a[4],size,PW_X86_WRITE))!=PW_OK)return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,result))!=PW_OK)return status;
        if(result!=PW_REG_ERROR_FILE_NOT_FOUND && result!=PW_REG_ERROR_INVALID_HANDLE &&
           result!=PW_REG_ERROR_INVALID_PARAMETER) {
            memcpy((void *)(uintptr_t)a[5],&size,4);
            if(a[3])memcpy((void *)(uintptr_t)a[3],&type,4);
            if(!result && a[4] && size)memcpy((void *)(uintptr_t)a[4],data,size);
        }
        *state=after;r->calls++;return PW_OK;
    }
    /* RegQueryValueA reads the unnamed value of a key or child key. */
    char subkey[PW_REG_PATH_MAX+1];uint8_t data[PW_REG_DATA_MAX];uint32_t size;
    if((status=guest_string(state,a[1],subkey,sizeof(subkey),1))!=PW_OK)return status;
    if((status=word_access(state,a[3],PW_X86_READ|PW_X86_WRITE))!=PW_OK)return status;
    memcpy(&size,(const void *)(uintptr_t)a[3],4);uint32_t handle=a[0],opened=0,result=0;
    if(*subkey){result=pw_registry_open(r->registry,a[0],subkey,&handle);opened=!result;}
    if(!result)result=pw_registry_query(r->registry,handle,"",NULL,a[2]?data:NULL,&size);
    if(opened)(void)pw_registry_close(r->registry,handle);
    if(!result && a[2] && size && (status=range_access(state,a[2],size,PW_X86_WRITE))!=PW_OK)return status;
    PwX86State after=*state;call.state=&after;
    if((status=pw_guest_call_finish(&call,32,result))!=PW_OK)return status;
    if(result!=PW_REG_ERROR_FILE_NOT_FOUND && result!=PW_REG_ERROR_INVALID_HANDLE &&
       result!=PW_REG_ERROR_INVALID_PARAMETER) {
        memcpy((void *)(uintptr_t)a[3],&size,4);
        if(!result && a[2] && size)memcpy((void *)(uintptr_t)a[2],data,size);
    }
    *state=after;r->calls++;return PW_OK;
}
static int cp1252(uint32_t c,uint8_t *out)
{
    static const uint16_t high[32]={0x20ac,0,0x201a,0x192,0x201e,0x2026,0x2020,0x2021,
        0x2c6,0x2030,0x160,0x2039,0x152,0,0x17d,0,0,0x2018,0x2019,0x201c,
        0x201d,0x2022,0x2013,0x2014,0x2dc,0x2122,0x161,0x203a,0x153,0,0x17e,0x178};
    if(c<0x80 || (c>=0xa0 && c<=0xff)){*out=(uint8_t)c;return PW_OK;}
    for(unsigned i=0;i<32;i++)if(c==high[i]){*out=(uint8_t)(0x80+i);return PW_OK;}
    return PW_ERR_UNSUPPORTED; /* best-fit/default-character policy not implemented */
}
static int copy_string(PwX86State *s,uint32_t dst,uint32_t src,uint32_t count,
                       unsigned bounded,unsigned append,uint32_t *write_at,uint32_t *length)
{
    uint32_t n=0,prefix=0;int status;
    if(bounded) {
        uint32_t limit=count-1;
        while(n<limit && n<0x100000) {
            if(src>UINT32_MAX-n)return PW_ERR_VM;
            if((status=range_access(s,src+n,1,PW_X86_READ))!=PW_OK)return status;
            if(!*(const uint8_t *)(uintptr_t)(src+n))break;
            n++;
        }
        if(n==0x100000 && n<limit)return PW_ERR_LIMIT;
    } else if((status=string_length(s,src,&n))!=PW_OK)return status;
    if(append && (status=string_length(s,dst,&prefix))!=PW_OK)return status;
    if(dst>UINT32_MAX-prefix)return PW_ERR_VM;
    uint32_t at=dst+prefix;
    if((status=range_access(s,at,(size_t)n+1,PW_X86_WRITE))!=PW_OK)return status;
    /* strcpy follows Wine's memmove behavior. The other two routines have
     * different forward-copy overlap semantics: reject overlapping spans
     * instead of silently implementing memmove for them. */
    uint64_t src_end=(uint64_t)src+n+(bounded?0:1),dst_end=(uint64_t)at+n+1;
    if((bounded || append) && n && src<dst_end && src_end>dst && !(bounded && src==dst))return PW_ERR_UNSUPPORTED;
    *write_at=at;*length=n;return PW_OK;
}
static uint8_t ascii_lower(uint8_t value)
{
    return value>='A' && value<='Z'?(uint8_t)(value+('a'-'A')):value;
}
static int compare_ascii_nocase(PwX86State *s,uint32_t first,uint32_t second,
                                uint32_t count,int32_t *result)
{
    if(!result)return PW_ERR_PRECONDITION;
    *result=0;
    for(uint32_t i=0;i<count;i++) {
        if(first>UINT32_MAX-i || second>UINT32_MAX-i)return PW_ERR_VM;
        int status=range_access(s,first+i,1,PW_X86_READ);
        if(status!=PW_OK)return status;
        if((status=range_access(s,second+i,1,PW_X86_READ))!=PW_OK)return status;
        uint8_t raw_a=*(const uint8_t *)(uintptr_t)(first+i);
        uint8_t raw_b=*(const uint8_t *)(uintptr_t)(second+i);
        uint8_t a=ascii_lower(raw_a),b=ascii_lower(raw_b);
        if(a!=b){*result=a<b?-1:1;return PW_OK;}
        if(!raw_a)return PW_OK;
    }
    return PW_OK;
}
static int table_read(PwX86State *s,uint32_t address,uint32_t *value)
{
    int status=word_access(s,address,PW_X86_READ);
    if(status==PW_OK)memcpy(value,(void *)(uintptr_t)address,4);
    return status;
}
static int init_next(PwWin32 *r,PwX86State *s)
{
    PwWin32Init *f=&r->init[r->init_depth-1];
    while(f->cursor<f->end) {
        uint32_t target;int status=table_read(s,f->cursor,&target);
        if(status!=PW_OK)return status;
        if(target) {
            /* Code permissions are verified by the execution dispatcher. */
            if(target>=PW_WIN32_TOKEN_BASE)return PW_ERR_UNSUPPORTED;
            status=pw_guest_callback_enter(&f->callback,s,target,
                PW_WIN32_CALLBACK_BASE+(r->init_depth-1)*16,NULL,0,PW_GUEST_CDECL);
            if(status!=PW_OK)return status;
            f->cursor+=4;r->callback_pending=1;return PW_OK;
        }
        f->cursor+=4;
    }
    int status=pw_guest_call_finish(&f->call,0,0);
    if(status==PW_OK){memset(f,0,sizeof(*f));r->init_depth--;r->calls++;}
    return status;
}
static int create_callback(PwWin32 *r,PwX86State *s,uint32_t message)
{
    if(!r->create_depth || r->create_depth>PW_WIN32_CREATE_DEPTH)return PW_ERR_STATE;
    unsigned index=r->create_depth-1;PwWin32Create *create=&r->create[index];
    uint32_t args[]={create->handle,message,0,create->create_struct};
    int status=pw_guest_callback_enter(&create->callback,s,create->wndproc,
        PW_WIN32_CREATE_CALLBACK_BASE+index*16,args,4,PW_GUEST_STDCALL);
    if(status==PW_OK)r->callback_pending=1;
    return status;
}
static int create_return(PwWin32 *r,PwX86State *s,unsigned index)
{
    if(!r->create_depth || index!=r->create_depth-1 || index>=PW_WIN32_CREATE_DEPTH)
        return PW_ERR_STATE;
    PwWin32Create *create=&r->create[index];uint64_t result;
    if(!create->active || create->callback.state!=s ||
       create->create_struct+48!=create->call.esp)return PW_ERR_STATE;
    int status=pw_guest_callback_leave(&create->callback,32,&result);
    if(status!=PW_OK)return status;
    if(create->phase==1 && (uint32_t)result) {
        create->phase=2;return create_callback(r,s,1); /* WM_CREATE */
    }
    unsigned commit=create->phase==2 && (uint32_t)result!=UINT32_MAX;
    PwX86State after=*s;after.gpr[4]=create->call.esp;
    PwGuestCall checked=create->call;checked.state=&after;
    if((status=pw_guest_call_finish(&checked,32,commit?create->handle:0))!=PW_OK)
        return status;
    if((status=pw_user32_finish_window(r->user32,create->slot,commit))!=PW_OK)return status;
    *s=after;memset(create,0,sizeof(*create));r->create_depth--;r->calls++;return PW_OK;
}
static int update_return(PwWin32 *r,PwX86State *s)
{
    PwWin32Update *update=&r->update;uint64_t ignored;
    if(!update->active || update->callback.state!=s)return PW_ERR_STATE;
    int status=pw_guest_callback_leave(&update->callback,32,&ignored);
    if(status!=PW_OK)return status;
    PwX86State after=*s;PwGuestCall checked=update->call;checked.state=&after;
    if((status=pw_guest_call_finish(&checked,32,1))!=PW_OK)return status;
    if((status=pw_user32_finish_paint(r->user32,update->handle))!=PW_OK)return status;
    *s=after;memset(update,0,sizeof(*update));r->calls++;return PW_OK;
}
static int message_dispatch_return(PwWin32 *r,PwX86State *s)
{
    PwWin32MessageDispatch *dispatch=&r->message_dispatch;uint64_t result;
    if(!dispatch->active || dispatch->callback.state!=s)return PW_ERR_STATE;
    int status=pw_guest_callback_leave(&dispatch->callback,32,&result);
    if(status!=PW_OK)return status;
    PwX86State after=*s;PwGuestCall checked=dispatch->call;checked.state=&after;
    if((status=pw_guest_call_finish(&checked,32,result))!=PW_OK)return status;
    *s=after;memset(dispatch,0,sizeof(*dispatch));r->calls++;return PW_OK;
}
static int gdi_failure(int status,uint32_t *result,uint32_t *error)
{
    if(status==PW_ERR_LIMIT){*result=0;*error=8;return PW_OK;}
    if(status==PW_ERR_NOT_FOUND){*result=0;*error=6;return PW_OK;}
    if(status==PW_ERR_PRECONDITION){*result=0;*error=87;return PW_OK;}
    if(status==PW_ERR_STATE){*result=0;return PW_OK;}
    return status;
}
static int gdi_dispatch(PwWin32 *r,PwX86State *state)
{
    unsigned user=!strcmp(r->last_dll,"user32.dll");
    unsigned gdi=!strcmp(r->last_dll,"gdi32.dll");
    if((!user && !gdi) ||
       (user && strcmp(r->last_name,"GetDC") && strcmp(r->last_name,"ReleaseDC")) ||
       (gdi && strcmp(r->last_name,"CreateCompatibleDC") &&
        strcmp(r->last_name,"CreatePalette") && strcmp(r->last_name,"SetPaletteEntries") &&
        strcmp(r->last_name,"GetStockObject") &&
        strcmp(r->last_name,"CreateCompatibleBitmap") && strcmp(r->last_name,"SelectObject") &&
        strcmp(r->last_name,"DeleteDC") && strcmp(r->last_name,"DeleteObject") &&
        strcmp(r->last_name,"GetLayout") && strcmp(r->last_name,"SetLayout") &&
        strcmp(r->last_name,"GetDeviceCaps") && strcmp(r->last_name,"GetObjectA") &&
        strcmp(r->last_name,"SelectPalette") && strcmp(r->last_name,"RealizePalette") &&
        strcmp(r->last_name,"SetSystemPaletteUse") && strcmp(r->last_name,"GetSystemPaletteEntries") &&
        strcmp(r->last_name,"ResizePalette") &&
        strcmp(r->last_name,"BitBlt") && strcmp(r->last_name,"StretchDIBits")))return PW_ERR_NOT_FOUND;
    if(!r->gdi || !r->user32)return PW_ERR_STATE;
    if(gdi && !strcmp(r->last_name,"CreatePalette")) {
        PwGuestCall call={0};uint32_t address;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,4,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&address))!=PW_OK ||
           (status=range_access(state,address,4,PW_X86_READ))!=PW_OK)return status;
        uint16_t header[2];memcpy(header,(const void *)(uintptr_t)address,4);
        if(header[0]!=0x300 || !header[1] || header[1]>PW_GDI_PALETTE_ENTRIES)
            return PW_ERR_UNSUPPORTED;
        if((status=range_access(state,address+4,(size_t)header[1]*4,PW_X86_READ))!=PW_OK)
            return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,0))!=PW_OK)return status;
        uint32_t result=0,error=0;
        status=pw_gdi_create_palette(r->gdi,header[0],header[1],
            (const uint8_t (*)[4])(uintptr_t)(address+4),&result);
        if(status!=PW_OK)status=gdi_failure(status,&result,&error);
        if(status!=PW_OK)return status;
        after.gpr[0]=result;*state=after;if(error)r->last_error=error;r->calls++;return PW_OK;
    }
    if(gdi && !strcmp(r->last_name,"SetPaletteEntries")) {
        PwGuestCall call={0};uint32_t a[4];
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,16,0);
        if(status!=PW_OK)return status;
        for(unsigned i=0;i<4;i++)if((status=pw_guest_call_u32(&call,i*4,&a[i]))!=PW_OK)return status;
        if(a[2] && (status=range_access(state,a[3],(size_t)a[2]*4,PW_X86_READ))!=PW_OK)return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,0))!=PW_OK)return status;
        uint32_t result=0,error=0;
        status=a[2]?pw_gdi_set_palette_entries(r->gdi,a[0],a[1],a[2],
            (const uint8_t (*)[4])(uintptr_t)a[3],&result):PW_OK;
        if(status!=PW_OK)status=gdi_failure(status,&result,&error);
        if(status!=PW_OK)return status;
        after.gpr[0]=result;*state=after;if(error)r->last_error=error;r->calls++;return PW_OK;
    }
    if(gdi && !strcmp(r->last_name,"GetStockObject")) {
        PwGuestCall call={0};uint32_t index,result=0,error=0;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,4,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&index))!=PW_OK)return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,0))!=PW_OK)return status;
        status=pw_gdi_stock_object(index,&result);
        if(status!=PW_OK)status=gdi_failure(status,&result,&error);
        if(status!=PW_OK)return status;
        after.gpr[0]=result;*state=after;if(error)r->last_error=error;r->calls++;return PW_OK;
    }
    unsigned count=!strcmp(r->last_name,"StretchDIBits")?13:
        !strcmp(r->last_name,"BitBlt")?9:
        !strcmp(r->last_name,"GetSystemPaletteEntries")?4:
        (!strcmp(r->last_name,"CreateCompatibleBitmap") || !strcmp(r->last_name,"GetObjectA") ||
         !strcmp(r->last_name,"SelectPalette"))?3:
        (!strcmp(r->last_name,"ReleaseDC") || !strcmp(r->last_name,"SelectObject") ||
         !strcmp(r->last_name,"SetLayout") || !strcmp(r->last_name,"GetDeviceCaps") ||
         !strcmp(r->last_name,"SetSystemPaletteUse") || !strcmp(r->last_name,"ResizePalette"))?2:1;
    PwGuestCall call={0};uint32_t a[13]={0};
    int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,count*4,0);
    if(status!=PW_OK)return status;
    for(unsigned i=0;i<count;i++)
        if((status=pw_guest_call_u32(&call,i*4,&a[i]))!=PW_OK)return status;
    if(gdi && !strcmp(r->last_name,"StretchDIBits")) {
        if((status=range_access(state,a[10],40,PW_X86_READ))!=PW_OK)return status;
        const uint8_t *info=(const uint8_t *)(uintptr_t)a[10];
        uint32_t header=load_le32(info),raw_width=load_le32(info+4),raw_height=load_le32(info+8);
        int32_t dib_width=(int32_t)raw_width,dib_height=(int32_t)raw_height;
        uint16_t bpp=load_le16(info+14);uint32_t colors=load_le32(info+32);
        if(header<40 || dib_width<=0 || !dib_height || dib_height==INT32_MIN ||
           (bpp!=8 && bpp!=24 && bpp!=32))return PW_ERR_UNSUPPORTED;
        uint32_t palette=bpp==8?(colors?colors:256):0;
        uint64_t info_bytes=(uint64_t)header+(uint64_t)palette*(a[11]==1?2:4);
        uint64_t stride=((uint64_t)(uint32_t)dib_width*bpp+31)/32*4;
        uint64_t bits_bytes=stride*(dib_height<0?(uint32_t)-dib_height:(uint32_t)dib_height);
        if(info_bytes>UINT32_MAX || bits_bytes>UINT32_MAX ||
           (status=range_access(state,a[10],(size_t)info_bytes,PW_X86_READ))!=PW_OK ||
           (status=range_access(state,a[9],(size_t)bits_bytes,PW_X86_READ))!=PW_OK)return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,0))!=PW_OK)return status;
        int32_t result=0;uint32_t error=0;
        status=pw_gdi_stretch_dibits(r->gdi,a[0],(int32_t)a[1],(int32_t)a[2],
            (int32_t)a[3],(int32_t)a[4],(int32_t)a[5],(int32_t)a[6],
            (int32_t)a[7],(int32_t)a[8],(const uint8_t *)(uintptr_t)a[9],(uint32_t)bits_bytes,
            info,(uint32_t)info_bytes,a[11],a[12],&result);
        if(status!=PW_OK)status=gdi_failure(status,(uint32_t *)&result,&error);
        if(status!=PW_OK)return status;
        after.gpr[0]=(uint32_t)result;*state=after;if(error)r->last_error=error;r->calls++;return PW_OK;
    }
    if(gdi && !strcmp(r->last_name,"GetSystemPaletteEntries")) {
        if(a[2] && (status=range_access(state,a[3],(size_t)a[2]*4,PW_X86_WRITE))!=PW_OK)return status;
        uint8_t entries[PW_GDI_PALETTE_ENTRIES][4];uint32_t result=0,error=0;
        status=pw_gdi_get_system_palette_entries(r->gdi,a[0],a[1],a[2],entries,&result);
        if(status!=PW_OK)status=gdi_failure(status,&result,&error);
        if(status!=PW_OK)return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,result))!=PW_OK)return status;
        if(result)memcpy((void *)(uintptr_t)a[3],entries,(size_t)result*4);
        *state=after;if(error)r->last_error=error;r->calls++;return PW_OK;
    }
    if(gdi && !strcmp(r->last_name,"GetObjectA")) {
        if(a[1]!=24)return PW_ERR_UNSUPPORTED;
        if((status=range_access(state,a[2],24,PW_X86_WRITE))!=PW_OK)return status;
        PwGdiBitmapInfo info;
        status=pw_gdi_bitmap_info(r->gdi,a[0],&info);
        uint32_t result=status==PW_OK?24:0,error=status==PW_ERR_NOT_FOUND?6:0;
        if(status!=PW_OK && status!=PW_ERR_NOT_FOUND)return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,result))!=PW_OK)return status;
        if(result) {
            uint8_t output[24]={0};
            memcpy(output+4,&info.width,4);memcpy(output+8,&info.height,4);
            memcpy(output+12,&info.stride,4);
            uint16_t planes=(uint16_t)info.planes,bits=(uint16_t)info.bits_per_pixel;
            memcpy(output+16,&planes,2);memcpy(output+18,&bits,2);
            memcpy((void *)(uintptr_t)a[2],output,sizeof(output));
        }
        *state=after;if(error)r->last_error=error;r->calls++;return PW_OK;
    }
    PwUser32Rect rect={0};
    if(user && !strcmp(r->last_name,"GetDC")) {
        status=pw_user32_get_window_rect(r->user32,a[0],&rect);
        if(status==PW_ERR_NOT_FOUND || status==PW_ERR_PRECONDITION) {
            PwX86State after=*state;call.state=&after;
            if((status=pw_guest_call_finish(&call,32,0))!=PW_OK)return status;
            *state=after;r->last_error=1400;r->calls++;return PW_OK;
        }
        if(status!=PW_OK)return status;
    }
    /* Validate stdcall cleanup before any process-owned GDI mutation. */
    PwX86State after=*state;call.state=&after;
    if((status=pw_guest_call_finish(&call,32,0))!=PW_OK)return status;
    uint32_t result=0,error=0;
    if(user && !strcmp(r->last_name,"GetDC")) {
        uint32_t width=(uint32_t)(rect.right-rect.left),height=(uint32_t)(rect.bottom-rect.top);
        status=pw_gdi_get_dc(r->gdi,a[0],width,height,&result);
    } else if(user) {
        status=pw_gdi_release_dc(r->gdi,a[0],a[1]);result=status==PW_OK;
    } else if(!strcmp(r->last_name,"CreateCompatibleDC")) {
        status=pw_gdi_create_compatible_dc(r->gdi,a[0],&result);
    } else if(!strcmp(r->last_name,"CreateCompatibleBitmap")) {
        status=(int32_t)a[1]<=0 || (int32_t)a[2]<=0?PW_ERR_PRECONDITION:
            pw_gdi_create_compatible_bitmap(r->gdi,a[0],a[1],a[2],&result);
    } else if(!strcmp(r->last_name,"SelectObject")) {
        status=pw_gdi_select_bitmap(r->gdi,a[0],a[1],&result);
    } else if(!strcmp(r->last_name,"DeleteDC")) {
        status=pw_gdi_delete_dc(r->gdi,a[0]);result=status==PW_OK;
    } else if(!strcmp(r->last_name,"DeleteObject")) {
        status=pw_gdi_delete_object(r->gdi,a[0]);result=status==PW_OK;
    } else if(!strcmp(r->last_name,"GetLayout")) {
        status=pw_gdi_get_layout(r->gdi,a[0],&result);
        if(status==PW_ERR_NOT_FOUND){status=PW_OK;result=UINT32_MAX;error=6;}
    } else if(!strcmp(r->last_name,"SetLayout")) {
        status=pw_gdi_set_layout(r->gdi,a[0],a[1],&result);
        if(status==PW_ERR_NOT_FOUND){status=PW_OK;result=UINT32_MAX;error=6;}
    } else if(!strcmp(r->last_name,"GetDeviceCaps")) {
        status=pw_gdi_get_device_caps(r->gdi,a[0],a[1],&result);
    } else if(!strcmp(r->last_name,"SelectPalette")) {
        status=pw_gdi_select_palette(r->gdi,a[0],a[1],a[2],&result);
    } else if(!strcmp(r->last_name,"RealizePalette")) {
        status=pw_gdi_realize_palette(r->gdi,a[0],&result);
    } else if(!strcmp(r->last_name,"SetSystemPaletteUse")) {
        status=pw_gdi_set_system_palette_use(r->gdi,a[0],a[1],&result);
    } else if(!strcmp(r->last_name,"ResizePalette")) {
        status=pw_gdi_resize_palette(r->gdi,a[0],a[1]);result=status==PW_OK;
    } else {
        status=(int32_t)a[3]<=0 || (int32_t)a[4]<=0?PW_ERR_PRECONDITION:
            pw_gdi_bitblt(r->gdi,a[0],(int32_t)a[1],(int32_t)a[2],a[3],a[4],
                          a[5],(int32_t)a[6],(int32_t)a[7],a[8]);
        result=status==PW_OK;
    }
    if(status!=PW_OK)status=gdi_failure(status,&result,&error);
    if(status!=PW_OK)return status;
    after.gpr[0]=result;*state=after;if(error)r->last_error=error;r->calls++;return PW_OK;
}
static int waveout_dispatch(PwWin32 *r,PwX86State *state)
{
    if(strcmp(r->last_dll,"winmm.dll"))return PW_ERR_NOT_FOUND;
    unsigned mm_open=!strcmp(r->last_name,"mmioOpenA"),mm_read=!strcmp(r->last_name,"mmioRead");
    unsigned mm_descend=!strcmp(r->last_name,"mmioDescend"),mm_ascend=!strcmp(r->last_name,"mmioAscend");
    unsigned mm_close=!strcmp(r->last_name,"mmioClose");
    if(mm_open || mm_read || mm_descend || mm_ascend || mm_close) {
        unsigned count=mm_descend?4:mm_close?2:3;PwGuestCall call={0};uint32_t a[4]={0};
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,count*4,0);
        if(status!=PW_OK)return status;
        for(unsigned i=0;i<count;i++)if((status=pw_guest_call_u32(&call,i*4,&a[i]))!=PW_OK)return status;
        PwMmio *stream=NULL;unsigned slot=8;uint32_t result=0;
        if(!mm_open)for(unsigned i=0;i<8;i++)if(r->mmio[i].open && r->mmio[i].handle==a[0]){stream=&r->mmio[i];break;}
        if(!mm_open && !stream)result=UINT32_MAX;
        if(mm_open) {
            if(!a[0] || a[1] || (a[2]&~0x10000u) || !r->services.file_open)return PW_ERR_UNSUPPORTED;
            for(unsigned i=0;i<8;i++)if(!r->mmio[i].open){slot=i;break;}
            if(slot==8)return PW_ERR_LIMIT;
        } else if(mm_read && a[2] &&
                  (status=range_access(state,a[1],a[2],PW_X86_WRITE))!=PW_OK)return status;
        else if((mm_descend || mm_ascend) &&
                (status=range_access(state,a[1],20,PW_X86_READ|PW_X86_WRITE))!=PW_OK)return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,result))!=PW_OK)return status;
        if(mm_open) {
            char path[PW_PATH_MAX+1];
            if((status=guest_string(state,a[0],path,sizeof(path),0))!=PW_OK)return status;
            uint32_t file=0;status=r->services.file_open(r->services.opaque,path,"rb",&file);
            if(status==PW_ERR_NOT_FOUND){status=PW_OK;result=0;}
            else if(status==PW_OK) {
                result=0x0f000001u+slot;r->mmio[slot]=(PwMmio){result,file,1};
            }
        } else if(result==UINT32_MAX)status=PW_OK;
        else if(mm_read) {
            uint32_t got=0;status=r->services.file_read(r->services.opaque,stream->file,
                (void *)(uintptr_t)a[1],a[2],&got);result=status==PW_OK?got:UINT32_MAX;
        } else if(mm_close) {
            status=r->services.file_close(r->services.opaque,stream->file);
            if(status==PW_OK){*stream=(PwMmio){0};result=0;}
        } else {
            uint8_t chunk[20];memcpy(chunk,(const void *)(uintptr_t)a[1],20);
            uint32_t ckid,cksize,type,offset,position=0;memcpy(&ckid,chunk,4);memcpy(&cksize,chunk+4,4);
            memcpy(&type,chunk+8,4);memcpy(&offset,chunk+12,4);
            if(mm_ascend) {
                uint64_t target=(uint64_t)offset+cksize+(cksize&1u);
                status=target>UINT32_MAX?PW_ERR_LIMIT:
                    r->services.file_seek(r->services.opaque,stream->file,(int32_t)target,0,&position);
                result=status==PW_OK?0:0x101;
            } else {
                uint32_t flags=a[3],wanted=flags==0x20?type:ckid;
                if(flags!=0x10 && flags!=0x20)return PW_ERR_UNSUPPORTED;
                uint32_t limit=UINT32_MAX;
                if(a[2]) {
                    uint8_t parent[20];
                    if((status=range_access(state,a[2],20,PW_X86_READ))!=PW_OK)return status;
                    memcpy(parent,(const void *)(uintptr_t)a[2],20);
                    uint32_t parent_size,parent_offset;memcpy(&parent_size,parent+4,4);memcpy(&parent_offset,parent+12,4);
                    if(parent_offset>UINT32_MAX-parent_size)return PW_ERR_MALFORMED;
                    limit=parent_offset+parent_size;
                }
                unsigned found=0;
                while(!found) {
                    uint8_t header[12];uint32_t got=0;
                    status=r->services.file_read(r->services.opaque,stream->file,header,flags==0x20?12:8,&got);
                    if(status!=PW_OK || got!=(flags==0x20?12u:8u)){result=0x101;status=PW_OK;break;}
                    memcpy(&ckid,header,4);memcpy(&cksize,header+4,4);type=0;
                    if(flags==0x20)memcpy(&type,header+8,4);
                    status=r->services.file_seek(r->services.opaque,stream->file,0,1,&position);
                    if(status!=PW_OK)return status;
                    if((flags==0x20 && ckid==0x46464952u && type==wanted) ||
                       (flags==0x10 && ckid==wanted)){found=1;offset=position;break;}
                    uint64_t next=(uint64_t)position+cksize+(cksize&1u);
                    if(next>limit || next>INT32_MAX){result=0x101;status=PW_OK;break;}
                    if((status=r->services.file_seek(r->services.opaque,stream->file,(int32_t)next,0,&position))!=PW_OK)return status;
                }
                if(found) {
                    memset(chunk,0,20);memcpy(chunk,&ckid,4);memcpy(chunk+4,&cksize,4);
                    memcpy(chunk+8,&type,4);memcpy(chunk+12,&offset,4);
                    memcpy((void *)(uintptr_t)a[1],chunk,20);result=0;
                }
            }
        }
        if(status!=PW_OK)return status;
        after.gpr[0]=result;*state=after;r->calls++;return PW_OK;
    }
    unsigned num=!strcmp(r->last_name,"waveOutGetNumDevs");
    unsigned caps=!strcmp(r->last_name,"waveOutGetDevCapsA");
    unsigned open=!strcmp(r->last_name,"waveOutOpen");
    unsigned play=!strcmp(r->last_name,"sndPlaySoundA");
    unsigned prepare=!strcmp(r->last_name,"waveOutPrepareHeader");
    unsigned write=!strcmp(r->last_name,"waveOutWrite");
    unsigned unprepare=!strcmp(r->last_name,"waveOutUnprepareHeader");
    unsigned pause=!strcmp(r->last_name,"waveOutPause");
    unsigned restart=!strcmp(r->last_name,"waveOutRestart");
    unsigned reset=!strcmp(r->last_name,"waveOutReset");
    unsigned close=!strcmp(r->last_name,"waveOutClose");
    unsigned position=!strcmp(r->last_name,"waveOutGetPosition");
    unsigned mci=!strcmp(r->last_name,"mciSendCommandA");
    if(!num && !caps && !open && !play && !prepare && !write && !unprepare &&
       !pause && !restart && !reset && !close && !position && !mci)return PW_ERR_NOT_FOUND;
    unsigned count=num?0:mci?4:(caps || prepare || write || unprepare || position)?3:
        play?2:(pause || restart || reset || close)?1:6;
    PwGuestCall call={0};uint32_t a[6]={0};
    int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,count*4,0);
    if(status!=PW_OK)return status;
    for(unsigned i=0;i<count;i++)
        if((status=pw_guest_call_u32(&call,i*4,&a[i]))!=PW_OK)return status;
    uint32_t result=0;uint8_t output[52]={0};size_t output_bytes=0;uint32_t output_address=0;
    PwWaveOut next=r->wave_out;
    if(num)result=1;
    else if(mci) {
        /* The first backend exposes PCM only. Reporting the sequencer as
         * unavailable is an ordinary MCI capability result, not a fabricated
         * successful MIDI device. Callers can continue with waveform audio. */
        r->mci_calls++;r->mci_last_command=a[1];
        result=0x107; /* MCIERR_DEVICE_NOT_INSTALLED */
    }
    else if(play) {
        if(a[0])return PW_ERR_UNSUPPORTED; /* named/resource playback service pending */
        result=1; /* NULL synchronously stops any legacy sndPlaySound stream */
    }
    else if(write || unprepare) {
        if(!next.open || a[0]!=next.handle)result=5;
        else if(a[2]!=32)result=11;
        else if((status=range_access(state,a[1],32,PW_X86_READ|PW_X86_WRITE))!=PW_OK)return status;
        else {
            uint32_t data,bytes,flags;memcpy(&data,(const void *)(uintptr_t)a[1],4);
            memcpy(&bytes,(const void *)(uintptr_t)(a[1]+4),4);
            memcpy(&flags,(const void *)(uintptr_t)(a[1]+16),4);
            unsigned index=next.header_count;
            for(unsigned i=0;i<next.header_count;i++)if(next.headers[i]==a[1]){index=i;break;}
            if(index==next.header_count || !(flags&2u))result=34; /* WAVERR_UNPREPARED */
            else if(unprepare) {
                if(flags&0x10u)result=33; /* WAVERR_STILLPLAYING */
                else {
                    memmove(&next.headers[index],&next.headers[index+1],
                            (next.header_count-index-1)*sizeof(next.headers[0]));
                    next.header_count--;flags&=~2u;
                    output_address=a[1]+16;output_bytes=4;memcpy(output,&flags,4);
                }
            } else if(!bytes || (status=range_access(state,data,bytes,PW_X86_READ))!=PW_OK) {
                if(status==PW_OK)result=11;else return status;
            } else if(next.paused)result=33;
            else {
                status=PW_OK;
                if(r->services.audio_submit)
                    status=r->services.audio_submit(r->services.opaque,
                        (const void *)(uintptr_t)data,bytes,a[1]);
                if(status==PW_ERR_LIMIT)result=7; /* MMSYSERR_NOMEM: bounded backpressure */
                else if(status!=PW_OK)return status;
                else {
                    next.bytes_submitted+=bytes;
                    flags=(flags|0x10u)&~1u; /* queued: INQUEUE, not DONE */
                    /* A backend without completion polling is a synchronous
                     * test sink. Real hosts provide audio_poll and complete later. */
                    if(!r->services.audio_poll) {
                        flags=(flags|1u)&~0x10u;next.bytes_completed+=bytes;
                        if(next.callback_flags==0x00010000u && r->user32) {
                            PwUser32QueueEntry done={.window=next.callback,.message=0x3bd,
                                .wparam=next.handle,.lparam=a[1]};
                            status=pw_user32_post_message(r->user32,&done);
                            if(status!=PW_OK && status!=PW_ERR_NOT_FOUND)return status;
                        }
                    }
                    output_address=a[1]+16;output_bytes=4;memcpy(output,&flags,4);
                }
            }
        }
    }
    else if(pause || restart || reset || close) {
        if(!next.open || a[0]!=next.handle)result=5;
        else {
            PwAudioControl operation=pause?PW_AUDIO_PAUSE:restart?PW_AUDIO_RESTART:
                reset?PW_AUDIO_RESET:PW_AUDIO_CLOSE;
            if(r->services.audio_control &&
               (status=r->services.audio_control(r->services.opaque,operation))!=PW_OK)return status;
            if(pause)next.paused=1;
            else if(restart)next.paused=0;
            else if(reset) {
                next.paused=0;next.bytes_completed=0;
                for(unsigned i=0;i<next.header_count;i++) {
                    uint32_t flags;
                    if((status=range_access(state,next.headers[i]+16,4,
                                             PW_X86_READ|PW_X86_WRITE))!=PW_OK)return status;
                    memcpy(&flags,(const void *)(uintptr_t)(next.headers[i]+16),4);
                    flags=(flags|1u)&~0x10u;
                    memcpy((void *)(uintptr_t)(next.headers[i]+16),&flags,4);
                }
            } else if(next.header_count)result=33;
            else next=(PwWaveOut){0};
        }
    }
    else if(caps) {
        if((a[0]!=0 && a[0]!=UINT32_MAX) || a[2]<52)result=2;
        else {
            output[0]=1;output[2]=1;output[4]=1;
            memcpy(output+8,"Prospero PCM",13);
            uint32_t formats=0x00000fffu;memcpy(output+40,&formats,4);
            output[44]=2;output_address=a[1];output_bytes=52;
            if((status=range_access(state,output_address,output_bytes,PW_X86_WRITE))!=PW_OK)return status;
        }
    } else if(position) {
        if(!next.open || a[0]!=next.handle)result=5;
        else if(a[2]<12)result=11;
        else {
            if((status=range_access(state,a[1],12,PW_X86_READ|PW_X86_WRITE))!=PW_OK)return status;
            uint32_t type;memcpy(&type,(const void *)(uintptr_t)a[1],4);
            uint64_t value=next.bytes_completed;
            if(type==1) /* TIME_MS */
                value=next.average_bytes_per_second?
                    value*1000/next.average_bytes_per_second:0;
            else if(type==2) /* TIME_SAMPLES */
                value=next.block_align?value/next.block_align:0;
            else type=4; /* TIME_BYTES; unsupported requests canonicalize to bytes */
            memcpy(output,&type,4);uint32_t narrowed=(uint32_t)value;
            memcpy(output+4,&narrowed,4);output_address=a[1];output_bytes=12;
        }
    } else if(prepare) {
        if(!next.open || a[0]!=next.handle)result=5;
        else if(a[2]!=32)result=11;
        else {
            if((status=range_access(state,a[1],32,PW_X86_READ|PW_X86_WRITE))!=PW_OK)return status;
            unsigned found=0;for(unsigned i=0;i<next.header_count;i++)found|=next.headers[i]==a[1];
            if(!found) {
                if(next.header_count==32)return PW_ERR_LIMIT;
                next.headers[next.header_count++]=a[1];
            }
            uint32_t flags;memcpy(&flags,(const void *)(uintptr_t)(a[1]+16),4);flags|=2u;
            output_address=a[1]+16;output_bytes=4;memcpy(output,&flags,4);
        }
    } else {
        if((status=range_access(state,a[2],18,PW_X86_READ))!=PW_OK)return status;
        uint8_t format[18];memcpy(format,(const void *)(uintptr_t)a[2],sizeof(format));
        uint16_t tag,channels,align,bits;uint32_t rate,average;
        memcpy(&tag,format,2);memcpy(&channels,format+2,2);memcpy(&rate,format+4,4);
        memcpy(&average,format+8,4);memcpy(&align,format+12,2);memcpy(&bits,format+14,2);
        uint32_t expected_align=(uint32_t)channels*bits/8;
        if(a[1]!=0 && a[1]!=UINT32_MAX)result=2;
        else if(tag!=1 || (channels!=1 && channels!=2) || (bits!=8 && bits!=16) ||
                !rate || align!=expected_align || average!=rate*expected_align)result=32;
        else if(a[5]&~0x00010001u)result=10;
        else if(a[5]&1u)result=0;
        else if(next.open)result=4;
        else if((a[5]&0x00010000u) && !a[3])result=11;
        else {
            output_address=a[0];output_bytes=4;
            if((status=range_access(state,output_address,output_bytes,PW_X86_WRITE))!=PW_OK)return status;
            next=(PwWaveOut){.handle=0x0e000001u,.callback=a[3],.callback_instance=a[4],
                .callback_flags=a[5]&0xffff0000u,.samples_per_second=rate,
                .average_bytes_per_second=average,.channels=channels,.block_align=align,
                .bits_per_sample=bits,.open=1};
            if(r->services.audio_open &&
               (status=r->services.audio_open(r->services.opaque,rate,channels,bits))!=PW_OK)
                return status;
            memcpy(output,&next.handle,4);
        }
    }
    PwX86State after=*state;call.state=&after;
    if((status=pw_guest_call_finish(&call,32,result))!=PW_OK)return status;
    if(output_bytes)memcpy((void *)(uintptr_t)output_address,output,output_bytes);
    r->wave_out=next;*state=after;r->calls++;return PW_OK;
}

int pw_win32_pump_audio(PwWin32 *r,PwX86State *state,uint32_t *completed)
{
    if(!r || !state || !completed)return PW_ERR_PRECONDITION;
    *completed=0;
    if(!r->services.audio_poll)return PW_OK;
    for(;;) {
        uint32_t header=0,bytes=0;
        int status=r->services.audio_poll(r->services.opaque,&header,&bytes);
        if(status==PW_ERR_NOT_FOUND)return PW_OK;
        if(status!=PW_OK)return status;
        unsigned found=0;
        for(unsigned i=0;i<r->wave_out.header_count;i++)found|=r->wave_out.headers[i]==header;
        if(!found)return PW_ERR_STATE;
        if((status=range_access(state,header+16,4,PW_X86_READ|PW_X86_WRITE))!=PW_OK)
            return status;
        uint32_t flags;memcpy(&flags,(const void *)(uintptr_t)(header+16),4);
        if(!(flags&0x10u))return PW_ERR_STATE;
        flags=(flags|1u)&~0x10u;
        memcpy((void *)(uintptr_t)(header+16),&flags,4);
        r->wave_out.bytes_completed+=bytes;
        if(r->wave_out.callback_flags==0x00010000u && r->user32) {
            PwUser32QueueEntry done={.window=r->wave_out.callback,.message=0x3bd,
                .wparam=r->wave_out.handle,.lparam=header};
            status=pw_user32_post_message(r->user32,&done);
            if(status!=PW_OK && status!=PW_ERR_NOT_FOUND)return status;
        }
        (*completed)++;
    }
}
int pw_win32_dispatch(PwWin32 *r,PwX86State *state)
{
    if(!r || !state)return PW_ERR_PRECONDITION;
    if(!r->main_base || !r->crt_data)return PW_ERR_STATE;
    r->callback_pending=0;r->idle_hint=0;
    if(state->eip==PW_WIN32_DISPATCH_CALLBACK) {
        r->last_dll="user32.dll";r->last_name="DispatchMessageA";
        return message_dispatch_return(r,state);
    }
    if(state->eip==PW_WIN32_UPDATE_CALLBACK) {
        r->last_dll="user32.dll";r->last_name="UpdateWindow";
        return update_return(r,state);
    }
    if(state->eip>=PW_WIN32_CREATE_CALLBACK_BASE &&
       state->eip<PW_WIN32_CREATE_CALLBACK_BASE+PW_WIN32_CREATE_DEPTH*16 &&
       !((state->eip-PW_WIN32_CREATE_CALLBACK_BASE)%16)) {
        r->last_dll="user32.dll";r->last_name="CreateWindowExA";
        return create_return(r,state,(state->eip-PW_WIN32_CREATE_CALLBACK_BASE)/16);
    }
    if(state->eip>=PW_WIN32_CALLBACK_BASE && state->eip<PW_WIN32_CALLBACK_BASE+PW_WIN32_INIT_DEPTH*16) {
        r->last_dll="msvcrt.dll";r->last_name="_initterm";
        if(!r->init_depth || state->eip!=PW_WIN32_CALLBACK_BASE+(r->init_depth-1)*16)return PW_ERR_STATE;
        PwWin32Init *f=&r->init[r->init_depth-1];
        if(f->callback.state!=state)return PW_ERR_STATE;
        uint64_t ignored;int status=pw_guest_callback_leave(&f->callback,0,&ignored);
        return status==PW_OK?init_next(r,state):status;
    }
    if(state->eip<PW_WIN32_TOKEN_BASE)return PW_ERR_NOT_FOUND;
    uint32_t offset=state->eip-PW_WIN32_TOKEN_BASE,index=offset/16;
    if(offset%16 || index>=sizeof(pw_catalog)/sizeof(pw_catalog[0]) ||
       pw_catalog[index].kind!=PW_IMPORT_FUNCTION)return PW_ERR_NOT_FOUND;
    r->last_dll=pw_catalog[index].dll;r->last_name=pw_catalog[index].name;
    if(!strcmp(r->last_dll,"advapi32.dll"))return registry_dispatch(r,state);
    int gdi_status=gdi_dispatch(r,state);
    if(gdi_status!=PW_ERR_NOT_FOUND)return gdi_status;
    int audio_status=waveout_dispatch(r,state);
    if(audio_status!=PW_ERR_NOT_FOUND)return audio_status;
    if(!strcmp(r->last_dll,"user32.dll") &&
       (!strcmp(r->last_name,"MapVirtualKeyA") || !strcmp(r->last_name,"GetKeyNameTextA"))) {
        if(!r->user32)return PW_ERR_STATE;
        unsigned key_name=!strcmp(r->last_name,"GetKeyNameTextA");
        unsigned count=key_name?3:2;PwGuestCall call={0};uint32_t a[3]={0};
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,count*4,0);
        if(status!=PW_OK)return status;
        for(unsigned i=0;i<count;i++)
            if((status=pw_guest_call_u32(&call,i*4,&a[i]))!=PW_OK)return status;
        char name[PW_USER32_NAME_MAX+1]={0};uint32_t result=0;
        if(key_name) {
            if(!a[2] || a[2]>sizeof(name))return PW_ERR_UNSUPPORTED;
            if((status=range_access(state,a[1],a[2],PW_X86_WRITE))!=PW_OK)return status;
            if((status=pw_user32_get_key_name(a[0],name,a[2],&result))!=PW_OK)return status;
        } else if((status=pw_user32_map_virtual_key(a[0],a[1],&result))!=PW_OK)return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,result))!=PW_OK)return status;
        if(key_name)memcpy((void *)(uintptr_t)a[1],name,(size_t)result+1);
        *state=after;r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"user32.dll") &&
       (!strcmp(r->last_name,"PeekMessageA") || !strcmp(r->last_name,"GetMessageA"))) {
        if(!r->user32)return PW_ERR_STATE;
        unsigned peek=!strcmp(r->last_name,"PeekMessageA");
        PwGuestCall call={0};uint32_t a[5]={0};unsigned count=peek?5:4;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,count*4,0);
        if(status!=PW_OK)return status;
        for(unsigned i=0;i<count;i++)
            if((status=pw_guest_call_u32(&call,i*4,&a[i]))!=PW_OK)return status;
        if((status=range_access(state,a[0],28,PW_X86_WRITE))!=PW_OK)return status;
        if(peek && (a[4]&~1u))return PW_ERR_UNSUPPORTED;
        PwUser32QueueEntry message;uint32_t found=0;
        status=pw_user32_peek_message(r->user32,a[1],a[2],a[3],peek?(a[4]&1u):1,
                                      &message,&found);
        uint32_t error=0,result=found;
        if(status==PW_ERR_NOT_FOUND){status=PW_OK;error=1400;result=0;}
        if(status!=PW_OK)return status;
        if(!peek && !found) {
            if(!r->services.message_wait)return PW_ERR_STATE;
            status=r->services.message_wait(r->services.opaque,a[1],&message);
            if(status!=PW_OK)return status;
            uint32_t wndproc;
            /* Message zero is WM_NULL, not an empty-provider sentinel. */
            if(!message.window ||
               pw_user32_window_proc(r->user32,message.window,&wndproc)!=PW_OK)
                return PW_ERR_STATE;
            if(a[1] && message.window!=a[1])return PW_ERR_STATE;
            if((a[2] || a[3]) && (message.message<a[2] || message.message>a[3]))
                return PW_ERR_STATE;
            found=result=1;
        }
        if(!peek && found && message.message==0x0012u)result=0; /* GetMessage(WM_QUIT) */
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,result))!=PW_OK)return status;
        if(found)memcpy((void *)(uintptr_t)a[0],&message,sizeof(message));
        *state=after;if(error)r->last_error=error;
        if(peek && !found)r->idle_hint=1;
        r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"user32.dll") && !strcmp(r->last_name,"PostQuitMessage")) {
        if(!r->user32)return PW_ERR_STATE;
        PwGuestCall call={0};uint32_t code;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,4,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&code))!=PW_OK)return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,0,0))!=PW_OK)return status;
        if((status=pw_user32_post_quit(r->user32,code))!=PW_OK)return status;
        *state=after;r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"user32.dll") && !strcmp(r->last_name,"PostMessageA")) {
        if(!r->user32)return PW_ERR_STATE;
        PwGuestCall call={0};uint32_t a[4];
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,16,0);
        if(status!=PW_OK)return status;
        for(unsigned i=0;i<4;i++)
            if((status=pw_guest_call_u32(&call,i*4,&a[i]))!=PW_OK)return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,0))!=PW_OK)return status;
        PwUser32QueueEntry message={.window=a[0],.message=a[1],.wparam=a[2],.lparam=a[3]};
        uint32_t result=1,error=0;
        status=pw_user32_post_message(r->user32,&message);
        if(status==PW_ERR_NOT_FOUND){status=PW_OK;result=0;error=1400;}
        else if(status==PW_ERR_LIMIT){status=PW_OK;result=0;error=8;}
        if(status!=PW_OK)return status;
        after.gpr[0]=result;*state=after;if(error)r->last_error=error;r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"user32.dll") && !strcmp(r->last_name,"TranslateMessage")) {
        PwGuestCall call={0};uint32_t address;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,4,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&address))!=PW_OK ||
           (status=range_access(state,address,28,PW_X86_READ))!=PW_OK)return status;
        status=pw_guest_call_finish(&call,32,0);if(status==PW_OK)r->calls++;return status;
    }
    if(!strcmp(r->last_dll,"user32.dll") && !strcmp(r->last_name,"DispatchMessageA")) {
        if(!r->user32 || r->message_dispatch.active)return PW_ERR_STATE;
        PwGuestCall call={0};uint32_t address;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,4,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&address))!=PW_OK ||
           (status=range_access(state,address,28,PW_X86_READ))!=PW_OK)return status;
        PwUser32QueueEntry message;memcpy(&message,(const void *)(uintptr_t)address,sizeof(message));
        /* A thread message such as WM_QUIT has no window and therefore no
         * procedure to run; DispatchMessage returns 0 for it. */
        if(!message.window)return pw_guest_call_finish(&call,32,0);
        uint32_t wndproc;
        if((status=pw_user32_window_proc(r->user32,message.window,&wndproc))!=PW_OK)return status;
        uint32_t args[]={message.window,message.message,message.wparam,message.lparam};
        r->message_dispatch=(PwWin32MessageDispatch){.call=call,.handle=message.window,
            .wndproc=wndproc,.active=1};
        status=pw_guest_callback_enter(&r->message_dispatch.callback,state,wndproc,
                                       PW_WIN32_DISPATCH_CALLBACK,args,4,PW_GUEST_STDCALL);
        if(status!=PW_OK){memset(&r->message_dispatch,0,sizeof(r->message_dispatch));return status;}
        r->callback_pending=1;return PW_OK;
    }
    if(!strcmp(r->last_dll,"user32.dll") &&
       (!strcmp(r->last_name,"RegisterWindowMessageA") || !strcmp(r->last_name,"FindWindowA"))) {
        if(!r->user32)return PW_ERR_STATE;
        unsigned find=!strcmp(r->last_name,"FindWindowA");PwGuestCall call={0};uint32_t a[2]={0};
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,find?8:4,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&a[0]))!=PW_OK ||
           (find && (status=pw_guest_call_u32(&call,4,&a[1]))!=PW_OK))return status;
        char first[PW_USER32_NAME_MAX+1],second[PW_USER32_NAME_MAX+1];uint32_t result=0;
        if((status=guest_string(state,a[0],first,sizeof(first),find))!=PW_OK)return status;
        if(find && (status=guest_string(state,a[1],second,sizeof(second),1))!=PW_OK)return status;
        /* Validate the return frame before a registration can mutate the
         * process-owned User32 namespace. */
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,0))!=PW_OK)return status;
        if(find)status=pw_user32_find_window(r->user32,a[0]?first:NULL,a[1]?second:NULL,&result);
        else status=pw_user32_register_message(r->user32,first,&result);
        uint32_t error=0;
        if(status==PW_ERR_PRECONDITION || status==PW_ERR_LIMIT){error=status==PW_ERR_LIMIT?8:87;result=0;status=PW_OK;}
        if(status!=PW_OK)return status;
        after.gpr[0]=result;
        *state=after;if(error)r->last_error=error;r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"comctl32.dll") && !strcmp(r->last_name,"InitCommonControlsEx")) {
        if(!r->user32)return PW_ERR_STATE;
        PwGuestCall call={0};uint32_t address,descriptor[2];
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,4,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&address))!=PW_OK ||
           (status=range_access(state,address,sizeof(descriptor),PW_X86_READ))!=PW_OK)return status;
        memcpy(descriptor,(const void *)(uintptr_t)address,sizeof(descriptor));
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,1))!=PW_OK)return status;
        status=pw_user32_init_common_controls(r->user32,descriptor[0],descriptor[1]);
        if(status!=PW_OK)return status;
        *state=after;r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"user32.dll") &&
       (!strcmp(r->last_name,"LoadIconA") || !strcmp(r->last_name,"LoadCursorA"))) {
        if(!r->user32 || !r->user32->resources)return PW_ERR_STATE;
        unsigned icon=!strcmp(r->last_name,"LoadIconA");
        PwGuestCall call={0};uint32_t module,resource;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,8,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&module))!=PW_OK ||
           (status=pw_guest_call_u32(&call,4,&resource))!=PW_OK)return status;
        char name[PW_USER32_NAME_MAX+1];const uint8_t *bytes=NULL;size_t size=0;
        if(icon) {
            if(module!=r->main_base || resource<0x10000u || !r->services.named_resource)
                return PW_ERR_UNSUPPORTED;
            if((status=guest_string(state,resource,name,sizeof(name),0))!=PW_OK)return status;
            status=r->services.named_resource(r->services.opaque,module,14,name,&bytes,&size);
            if(status!=PW_OK && status!=PW_ERR_NOT_FOUND)return status;
            if(status==PW_OK && (!bytes || !size || size>UINT32_MAX))return PW_ERR_STATE;
        } else if(module || (resource!=32512u && resource!=32514u)) {
            return PW_ERR_UNSUPPORTED; /* startup uses IDC_ARROW and IDC_WAIT */
        }
        /* Validate the stdcall return before allocating a process object. */
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,0))!=PW_OK)return status;
        uint32_t handle=0,error=0;
        if(icon && !bytes)error=1814; /* ERROR_RESOURCE_NAME_NOT_FOUND */
        else {
            status=pw_user32_resource(r->user32,
                icon?PW_USER32_ICON:PW_USER32_SYSTEM_CURSOR,
                icon?module:0,icon?14:0,icon?name:NULL,icon?0:resource,
                bytes,(uint32_t)size,&handle);
            if(status==PW_ERR_LIMIT){status=PW_OK;error=8;handle=0;}
            if(status!=PW_OK)return status;
        }
        after.gpr[0]=handle;*state=after;
        if(error)r->last_error=error;
        r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"user32.dll") && !strcmp(r->last_name,"LoadBitmapA")) {
        if(!r->gdi || !r->services.named_resource)return PW_ERR_STATE;
        PwGuestCall call={0};uint32_t module,resource;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,8,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&module))!=PW_OK ||
           (status=pw_guest_call_u32(&call,4,&resource))!=PW_OK)return status;
        if(module!=r->main_base || resource<0x10000u)return PW_ERR_UNSUPPORTED;
        char name[PW_USER32_NAME_MAX+1];
        if((status=guest_string(state,resource,name,sizeof(name),0))!=PW_OK)return status;
        const uint8_t *bytes=NULL;size_t size=0;
        status=r->services.named_resource(r->services.opaque,module,2,name,&bytes,&size);
        if(status!=PW_OK && status!=PW_ERR_NOT_FOUND)return status;
        if(status==PW_OK && (!bytes || !size || size>UINT32_MAX))return PW_ERR_STATE;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,0))!=PW_OK)return status;
        uint32_t handle=0,error=0;
        if(!bytes)error=1814;
        else {
            status=pw_gdi_create_dib_bitmap(r->gdi,bytes,(uint32_t)size,&handle);
            if(status==PW_ERR_LIMIT){status=PW_OK;error=8;}
            if(status!=PW_OK)return status;
        }
        after.gpr[0]=handle;*state=after;if(error)r->last_error=error;r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"user32.dll") && !strcmp(r->last_name,"RegisterClassA")) {
        if(!r->user32 || !r->user32->classes || !r->services.code_address)return PW_ERR_STATE;
        PwGuestCall call={0};uint32_t address,raw[10];
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,4,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&address))!=PW_OK ||
           (status=range_access(state,address,sizeof(raw),PW_X86_READ))!=PW_OK)return status;
        memcpy(raw,(const void *)(uintptr_t)address,sizeof(raw));
        if(raw[4]!=r->main_base)return PW_ERR_UNSUPPORTED;
        if((status=r->services.code_address(r->services.opaque,raw[1]))!=PW_OK)return status;
        char menu[PW_USER32_NAME_MAX+1],name[PW_USER32_NAME_MAX+1];
        if((status=guest_string(state,raw[8],menu,sizeof(menu),1))!=PW_OK ||
           (status=guest_string(state,raw[9],name,sizeof(name),0))!=PW_OK)return status;
        unsigned have_icon=!raw[5],have_cursor=!raw[6];
        for(uint32_t i=0;i<r->user32->resource_capacity;i++) {
            const PwUser32Resource *resource=&r->user32->resources[i];
            if(resource->used && resource->handle==raw[5] && resource->kind==PW_USER32_ICON)
                have_icon=1;
            if(resource->used && resource->handle==raw[6] && resource->kind==PW_USER32_SYSTEM_CURSOR)
                have_cursor=1;
        }
        if(!have_icon || !have_cursor)return PW_ERR_UNSUPPORTED;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,0))!=PW_OK)return status;
        PwUser32Class descriptor={0};
        memcpy(descriptor.menu_name,menu,strlen(menu)+1);
        memcpy(descriptor.class_name,name,strlen(name)+1);
        descriptor.style=raw[0];descriptor.wndproc=raw[1];descriptor.class_extra=raw[2];
        descriptor.window_extra=raw[3];descriptor.module=raw[4];descriptor.icon=raw[5];
        descriptor.cursor=raw[6];descriptor.background=raw[7];
        uint16_t atom=0;status=pw_user32_register_class(r->user32,&descriptor,&atom);
        uint32_t error=0;
        if(status==PW_ERR_STATE){status=PW_OK;error=1410;atom=0;}
        else if(status==PW_ERR_LIMIT){status=PW_OK;error=8;atom=0;}
        if(status!=PW_OK)return status;
        after.gpr[0]=atom;*state=after;if(error)r->last_error=error;
        r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"user32.dll") && !strcmp(r->last_name,"DefWindowProcA")) {
        if(!r->user32)return PW_ERR_STATE;
        PwGuestCall call={0};uint32_t args[4];
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,16,0);
        if(status!=PW_OK)return status;
        for(unsigned i=0;i<4;i++)if((status=pw_guest_call_u32(&call,i*4,&args[i]))!=PW_OK)return status;
        unsigned known=0;
        for(uint32_t i=0;i<r->user32->window_capacity;i++)
            if(r->user32->windows[i].used && r->user32->windows[i].handle==args[0])known=1;
        if(!known || args[1]>0xffff)return PW_ERR_UNSUPPORTED;
        /* Default processing is side-effect free in the headless model.
           WM_NCCREATE is the one creation message whose documented default
           must be TRUE; ordinary unhandled messages return zero. */
        status=pw_guest_call_finish(&call,32,args[1]==0x81?1:0);
        if(status==PW_OK)r->calls++;
        return status;
    }
    if(!strcmp(r->last_dll,"user32.dll") && !strcmp(r->last_name,"CreateWindowExA")) {
        if(!r->user32 || !r->user32->classes || r->create_depth>=PW_WIN32_CREATE_DEPTH)
            return PW_ERR_STATE;
        PwGuestCall call={0};uint32_t args[12];
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,48,0);
        if(status!=PW_OK)return status;
        for(unsigned i=0;i<12;i++)if((status=pw_guest_call_u32(&call,i*4,&args[i]))!=PW_OK)return status;
        if(args[1]<0x10000u || args[8] || args[9] ||
           args[10]!=r->main_base || args[11])return PW_ERR_UNSUPPORTED;
        char class_name[PW_USER32_NAME_MAX+1],title[PW_USER32_NAME_MAX+1];
        if((status=guest_string(state,args[1],class_name,sizeof(class_name),0))!=PW_OK ||
           (status=guest_string(state,args[2],title,sizeof(title),1))!=PW_OK)return status;
        const PwUser32Class *class_record=NULL;
        if((status=pw_user32_find_class(r->user32,class_name,args[10],&class_record))!=PW_OK) {
            if(status!=PW_ERR_NOT_FOUND)return status;
            PwX86State after=*state;call.state=&after;
            if((status=pw_guest_call_finish(&call,32,0))!=PW_OK)return status;
            *state=after;r->last_error=1407;r->calls++;return PW_OK;
        }
        PwX86State finish=*state;PwGuestCall checked=call;checked.state=&finish;
        if((status=pw_guest_call_finish(&checked,32,0))!=PW_OK)return status;
        if(call.esp<state->stack_low+48)return PW_ERR_VM;
        uint32_t scratch=call.esp-48;
        PwUser32Window window={.wndproc=class_record->wndproc,.ex_style=args[0],
            .style=args[3],.x=args[4],.y=args[5],.width=args[6],.height=args[7],
            .parent=args[8],.menu=args[9],.module=args[10],.param=args[11],
            .extra_bytes=class_record->window_extra};
        memcpy(window.class_name,class_name,strlen(class_name)+1);
        memcpy(window.title,title,strlen(title)+1);
        uint32_t slot,handle;
        status=pw_user32_begin_window(r->user32,&window,&slot,&handle);
        if(status==PW_ERR_LIMIT) {
            PwX86State after=*state;call.state=&after;
            if((status=pw_guest_call_finish(&call,32,0))!=PW_OK)return status;
            *state=after;r->last_error=8;r->calls++;return PW_OK;
        }
        if(status!=PW_OK)return status;
        if(class_record->menu_name[0]) {
            uint32_t menu_handle;
            status=pw_user32_attach_menu(r->user32,slot,&menu_handle);
            if(status!=PW_OK){(void)pw_user32_finish_window(r->user32,slot,0);return status;}
        }
        uint32_t create_struct[]={args[11],args[10],args[9],args[8],args[7],args[6],
            args[5],args[4],args[3],args[2],args[1],args[0]};
        memcpy((void *)(uintptr_t)scratch,create_struct,sizeof(create_struct));
        state->gpr[4]=scratch;
        unsigned create_index=r->create_depth++;
        r->create[create_index]=(PwWin32Create){.call=call,.slot=slot,.handle=handle,
            .wndproc=class_record->wndproc,.create_struct=scratch,.phase=1,.active=1};
        status=create_callback(r,state,0x81); /* WM_NCCREATE */
        if(status!=PW_OK) {
            (void)pw_user32_finish_window(r->user32,slot,0);
            memset(&r->create[create_index],0,sizeof(r->create[create_index]));
            r->create_depth--;state->gpr[4]=call.esp;
        }
        return status;
    }
    if(!strcmp(r->last_dll,"user32.dll") && !strcmp(r->last_name,"GetMenu")) {
        if(!r->user32)return PW_ERR_STATE;
        PwGuestCall call={0};uint32_t window,menu=0;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,4,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&window))!=PW_OK)return status;
        status=pw_user32_get_menu(r->user32,window,&menu);
        if(status==PW_ERR_NOT_FOUND){status=PW_OK;r->last_error=1400;menu=0;}
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_finish(&call,32,menu))!=PW_OK)return status;
        r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"user32.dll") && !strcmp(r->last_name,"GetSystemMetrics")) {
        if(!r->user32 || !r->user32->desktop_configured)return PW_ERR_STATE;
        PwGuestCall call={0};uint32_t index,value;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,4,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&index))!=PW_OK)return status;
        switch(index) {
        case 0:case 16:value=r->user32->desktop_width;break;  /* CXSCREEN/FULLSCREEN */
        case 1:case 17:value=r->user32->desktop_height;break; /* CYSCREEN/FULLSCREEN */
        case 2:case 3:value=17;break;                         /* scroll bars */
        case 4:value=23;break;                               /* CYCAPTION */
        case 5:case 6:value=1;break;                         /* border */
        case 7:case 8:value=3;break;                         /* dialog frame */
        case 15:value=19;break;                              /* CYMENU */
        case 32:case 33:value=4;break;                       /* sizing frame */
        default:return PW_ERR_UNSUPPORTED;
        }
        if((status=pw_guest_call_finish(&call,32,value))!=PW_OK)return status;
        r->calls++;return PW_OK;
    }
    unsigned check_menu=!strcmp(r->last_dll,"user32.dll") &&
        !strcmp(r->last_name,"CheckMenuItem");
    unsigned enable_menu=!strcmp(r->last_dll,"user32.dll") &&
        !strcmp(r->last_name,"EnableMenuItem");
    if(check_menu || enable_menu) {
        if(!r->user32)return PW_ERR_STATE;
        PwGuestCall call={0};uint32_t menu,item,flags,previous=UINT32_MAX;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,12,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&menu))!=PW_OK ||
           (status=pw_guest_call_u32(&call,4,&item))!=PW_OK ||
           (status=pw_guest_call_u32(&call,8,&flags))!=PW_OK)return status;
        uint32_t mask=check_menu?8u:3u;
        if(flags&~mask)return PW_ERR_UNSUPPORTED; /* command identifiers only */
        status=pw_user32_menu_item(r->user32,menu,item,mask,flags&mask,&previous);
        if(status==PW_ERR_NOT_FOUND){status=PW_OK;previous=UINT32_MAX;r->last_error=1401;}
        if(status==PW_ERR_LIMIT){status=PW_OK;previous=UINT32_MAX;r->last_error=8;}
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_finish(&call,32,previous))!=PW_OK)return status;
        r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"user32.dll") && !strcmp(r->last_name,"DeleteMenu")) {
        if(!r->user32)return PW_ERR_STATE;
        PwGuestCall call={0};uint32_t menu,item,flags,result=1;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,12,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&menu))!=PW_OK ||
           (status=pw_guest_call_u32(&call,4,&item))!=PW_OK ||
           (status=pw_guest_call_u32(&call,8,&flags))!=PW_OK)return status;
        if(flags)return PW_ERR_UNSUPPORTED; /* MF_BYCOMMAND */
        status=pw_user32_delete_menu(r->user32,menu,item);
        if(status==PW_ERR_NOT_FOUND){status=PW_OK;result=0;r->last_error=1401;}
        if(status==PW_ERR_LIMIT){status=PW_OK;result=0;r->last_error=8;}
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_finish(&call,32,result))!=PW_OK)return status;
        r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"user32.dll") && !strcmp(r->last_name,"DrawMenuBar")) {
        if(!r->user32)return PW_ERR_STATE;
        PwGuestCall call={0};uint32_t window,result=1;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,4,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&window))!=PW_OK)return status;
        status=pw_user32_draw_menu_bar(r->user32,window);
        if(status==PW_ERR_NOT_FOUND){status=PW_OK;result=0;r->last_error=1400;}
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_finish(&call,32,result))!=PW_OK)return status;
        r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"user32.dll") && !strcmp(r->last_name,"SetWindowLongA")) {
        if(!r->user32)return PW_ERR_STATE;
        PwGuestCall call={0};uint32_t args[3];
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,12,0);
        if(status!=PW_OK)return status;
        for(unsigned i=0;i<3;i++)
            if((status=pw_guest_call_u32(&call,i*4,&args[i]))!=PW_OK)return status;
        /* Preflight the guest return before mutating per-window storage. */
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,0))!=PW_OK)return status;
        uint32_t previous=0,error=0;
        status=pw_user32_set_window_long(r->user32,args[0],(int32_t)args[1],args[2],&previous);
        if(status==PW_ERR_NOT_FOUND){status=PW_OK;error=1400;} /* ERROR_INVALID_WINDOW_HANDLE */
        else if(status==PW_ERR_PRECONDITION){status=PW_OK;error=1413;} /* ERROR_INVALID_INDEX */
        if(status!=PW_OK)return status;
        after.gpr[0]=previous;*state=after;
        if(error)r->last_error=error;
        r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"user32.dll") && !strcmp(r->last_name,"GetWindowLongA")) {
        if(!r->user32)return PW_ERR_STATE;
        PwGuestCall call={0};uint32_t handle,index;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,8,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&handle))!=PW_OK ||
           (status=pw_guest_call_u32(&call,4,&index))!=PW_OK)return status;
        uint32_t result=0,error=0;
        status=pw_user32_get_window_long(r->user32,handle,(int32_t)index,&result);
        if(status==PW_ERR_NOT_FOUND){status=PW_OK;error=1400;}
        else if(status==PW_ERR_PRECONDITION){status=PW_OK;error=1413;}
        if(status!=PW_OK)return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,result))!=PW_OK)return status;
        *state=after;if(error)r->last_error=error;r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"user32.dll") && !strcmp(r->last_name,"GetDesktopWindow")) {
        if(!r->user32 || !r->user32->desktop_configured)return PW_ERR_STATE;
        PwGuestCall call={0};int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,0,0);
        if(status==PW_OK)status=pw_guest_call_finish(&call,32,PW_USER32_DESKTOP_HANDLE);
        if(status==PW_OK)r->calls++;
        return status;
    }
    if(!strcmp(r->last_dll,"user32.dll") && !strcmp(r->last_name,"GetWindowRect")) {
        if(!r->user32)return PW_ERR_STATE;
        PwGuestCall call={0};uint32_t handle,address;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,8,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&handle))!=PW_OK ||
           (status=pw_guest_call_u32(&call,4,&address))!=PW_OK ||
           (status=range_access(state,address,sizeof(PwUser32Rect),PW_X86_WRITE))!=PW_OK)
            return status;
        PwUser32Rect rect;
        status=pw_user32_get_window_rect(r->user32,handle,&rect);
        uint32_t result=1,error=0;
        if(status==PW_ERR_NOT_FOUND){status=PW_OK;result=0;error=1400;}
        else if(status==PW_ERR_PRECONDITION){status=PW_OK;result=0;error=87;}
        if(status!=PW_OK)return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,result))!=PW_OK)return status;
        if(result)memcpy((void *)(uintptr_t)address,&rect,sizeof(rect));
        *state=after;if(error)r->last_error=error;r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"user32.dll") && !strcmp(r->last_name,"MoveWindow")) {
        if(!r->user32 || !r->gdi)return PW_ERR_STATE;
        PwGuestCall call={0};uint32_t args[6];
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,24,0);
        if(status!=PW_OK)return status;
        for(unsigned i=0;i<6;i++)
            if((status=pw_guest_call_u32(&call,i*4,&args[i]))!=PW_OK)return status;
        if(args[5])return PW_ERR_UNSUPPORTED; /* source-confirmed splash path passes FALSE */
        PwUser32Rect old;
        status=pw_user32_get_window_rect(r->user32,args[0],&old);
        if(status==PW_OK) {
            int64_t right=(int64_t)(int32_t)args[1]+(int32_t)args[3];
            int64_t bottom=(int64_t)(int32_t)args[2]+(int32_t)args[4];
            if((int32_t)args[3]<=0 || (int32_t)args[4]<=0 || right<INT32_MIN ||
               right>INT32_MAX || bottom<INT32_MIN || bottom>INT32_MAX)
                status=PW_ERR_PRECONDITION;
        }
        uint32_t result=status==PW_OK,error=0;
        if(status==PW_ERR_NOT_FOUND){status=PW_OK;error=1400;}
        else if(status==PW_ERR_PRECONDITION || status==PW_ERR_LIMIT){status=PW_OK;error=87;}
        if(status!=PW_OK)return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,result))!=PW_OK)return status;
        if(result) {
            status=pw_gdi_resize_target(r->gdi,args[0],args[3],args[4]);
            if(status!=PW_OK)return status;
            status=pw_user32_move_window(r->user32,args[0],(int32_t)args[1],(int32_t)args[2],
                                         args[3],args[4]);
            if(status!=PW_OK)return status;
        }
        *state=after;if(error)r->last_error=error;r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"user32.dll") &&
       (!strcmp(r->last_name,"ShowWindow") || !strcmp(r->last_name,"SetFocus"))) {
        if(!r->user32)return PW_ERR_STATE;
        unsigned show=!strcmp(r->last_name,"ShowWindow");PwGuestCall call={0};uint32_t args[2]={0};
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,show?8:4,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&args[0]))!=PW_OK ||
           (show && (status=pw_guest_call_u32(&call,4,&args[1]))!=PW_OK))return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,0))!=PW_OK)return status;
        uint32_t result=0,error=0;
        status=show?pw_user32_show_window(r->user32,args[0],args[1],&result):
                    pw_user32_set_focus(r->user32,args[0],&result);
        if(status==PW_ERR_NOT_FOUND){status=PW_OK;error=1400;result=0;}
        if(status!=PW_OK)return status;
        after.gpr[0]=result;*state=after;if(error)r->last_error=error;r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"user32.dll") && !strcmp(r->last_name,"DestroyWindow")) {
        if(!r->user32 || !r->gdi)return PW_ERR_STATE;
        PwGuestCall call={0};uint32_t handle;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,4,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&handle))!=PW_OK)return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,0))!=PW_OK)return status;
        uint32_t result=1,error=0;
        status=pw_gdi_destroy_target(r->gdi,handle);
        if(status==PW_OK)status=pw_user32_destroy_window(r->user32,handle);
        if(status==PW_ERR_NOT_FOUND){status=PW_OK;result=0;error=1400;}
        if(status!=PW_OK)return status;
        after.gpr[0]=result;*state=after;if(error)r->last_error=error;r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"user32.dll") && !strcmp(r->last_name,"UnregisterClassA")) {
        if(!r->user32)return PW_ERR_STATE;
        PwGuestCall call={0};uint32_t name_address,module;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,8,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&name_address))!=PW_OK ||
           (status=pw_guest_call_u32(&call,4,&module))!=PW_OK)return status;
        char name[PW_USER32_NAME_MAX+1];
        if((status=guest_string(state,name_address,name,sizeof(name),0))!=PW_OK)return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,0))!=PW_OK)return status;
        uint32_t result=1,error=0;
        status=pw_user32_unregister_class(r->user32,name,module);
        if(status==PW_ERR_NOT_FOUND){status=PW_OK;result=0;error=1411;}
        else if(status==PW_ERR_STATE){status=PW_OK;result=0;error=1412;}
        if(status!=PW_OK)return status;
        after.gpr[0]=result;*state=after;if(error)r->last_error=error;r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"user32.dll") && !strcmp(r->last_name,"SetCursor")) {
        if(!r->user32)return PW_ERR_STATE;
        PwGuestCall call={0};uint32_t handle;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,4,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&handle))!=PW_OK)return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,0))!=PW_OK)return status;
        uint32_t result=0;
        status=pw_user32_set_cursor(r->user32,handle,&result);
        if(status==PW_ERR_NOT_FOUND){status=PW_OK;result=0;r->last_error=6;}
        if(status!=PW_OK)return status;
        after.gpr[0]=result;*state=after;r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"user32.dll") && !strcmp(r->last_name,"UpdateWindow")) {
        if(!r->user32 || r->update.active)return PW_ERR_STATE;
        PwGuestCall call={0};uint32_t handle;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,4,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&handle))!=PW_OK)return status;
        uint32_t wndproc=0,needed=0;
        status=pw_user32_paint_info(r->user32,handle,&wndproc,&needed);
        if(status==PW_ERR_NOT_FOUND) {
            PwX86State after=*state;call.state=&after;
            if((status=pw_guest_call_finish(&call,32,0))!=PW_OK)return status;
            *state=after;r->last_error=1400;r->calls++;return PW_OK;
        }
        if(status!=PW_OK)return status;
        if(!needed) {
            status=pw_guest_call_finish(&call,32,1);if(status==PW_OK)r->calls++;return status;
        }
        uint32_t args[]={handle,0x0f,0,0};
        r->update=(PwWin32Update){.call=call,.handle=handle,.wndproc=wndproc,.active=1};
        status=pw_guest_callback_enter(&r->update.callback,state,wndproc,
                                       PW_WIN32_UPDATE_CALLBACK,args,4,PW_GUEST_STDCALL);
        if(status!=PW_OK){memset(&r->update,0,sizeof(r->update));return status;}
        r->callback_pending=1;return PW_OK;
    }
    if(!strcmp(r->last_dll,"user32.dll") && !strcmp(r->last_name,"BeginPaint")) {
        if(!r->user32 || !r->gdi)return PW_ERR_STATE;
        PwGuestCall call={0};uint32_t handle,address;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,8,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&handle))!=PW_OK ||
           (status=pw_guest_call_u32(&call,4,&address))!=PW_OK ||
           (status=range_access(state,address,64,PW_X86_WRITE))!=PW_OK)return status;
        PwUser32Rect rect;uint32_t wndproc,needed;
        if((status=pw_user32_get_window_rect(r->user32,handle,&rect))!=PW_OK ||
           (status=pw_user32_paint_info(r->user32,handle,&wndproc,&needed))!=PW_OK)return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,0))!=PW_OK)return status;
        uint32_t dc,width=(uint32_t)(rect.right-rect.left),height=(uint32_t)(rect.bottom-rect.top);
        if((status=pw_gdi_get_dc(r->gdi,handle,width,height,&dc))!=PW_OK)return status;
        status=pw_user32_begin_paint(r->user32,handle,dc);
        if(status!=PW_OK){(void)pw_gdi_release_dc(r->gdi,handle,dc);return status;}
        uint8_t output[64]={0};
        memcpy(output,&dc,4);memcpy(output+16,&width,4);memcpy(output+20,&height,4);
        memcpy((void *)(uintptr_t)address,output,sizeof(output));
        after.gpr[0]=dc;*state=after;r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"user32.dll") && !strcmp(r->last_name,"EndPaint")) {
        if(!r->user32 || !r->gdi)return PW_ERR_STATE;
        PwGuestCall call={0};uint32_t handle,address,dc;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,8,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&handle))!=PW_OK ||
           (status=pw_guest_call_u32(&call,4,&address))!=PW_OK ||
           (status=range_access(state,address,64,PW_X86_READ))!=PW_OK)return status;
        memcpy(&dc,(const void *)(uintptr_t)address,4);
        if((status=pw_user32_check_paint(r->user32,handle,dc))!=PW_OK)return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,1))!=PW_OK)return status;
        if((status=pw_gdi_release_dc(r->gdi,handle,dc))!=PW_OK ||
           (status=pw_user32_end_paint(r->user32,handle,dc))!=PW_OK)return status;
        *state=after;r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"user32.dll") && !strcmp(r->last_name,"LoadStringA")) {
        if(!r->services.string_resource || r->services.ansi_codepage!=1252)return PW_ERR_UNSUPPORTED;
        PwGuestCall call={0};uint32_t arg[4];
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,16,0);
        if(status!=PW_OK)return status;
        for(unsigned i=0;i<4;i++)if((status=pw_guest_call_u32(&call,i*4,&arg[i]))!=PW_OK)return status;
        if(!arg[3] || arg[3]>4096)return PW_ERR_UNSUPPORTED;
        const uint8_t *text=NULL;size_t units=0;
        status=r->services.string_resource(r->services.opaque,arg[0],arg[1],&text,&units);
        if(status!=PW_OK && status!=PW_ERR_NOT_FOUND)return status;
        unsigned missing=status==PW_ERR_NOT_FOUND;
        if(!missing && ((!text && units) || units>65535))return PW_ERR_STATE;
        uint8_t output[4096];size_t n=0;
        if(!missing) {
            while(n<units && n<arg[3]-1) {
                uint32_t c=(uint32_t)text[n*2]|(uint32_t)text[n*2+1]<<8;
                if((status=cp1252(c,output+n))!=PW_OK)return status;
                n++;
            }
            output[n]=0;
            if((status=range_access(state,arg[2],n+1,PW_X86_WRITE))!=PW_OK)return status;
        }
        status=pw_guest_call_finish(&call,32,n);
        if(status!=PW_OK)return status;
        if(!missing)memcpy((void *)(uintptr_t)arg[2],output,n+1);
        r->calls++;return PW_OK;
    }
    unsigned kernel=!strcmp(r->last_dll,"kernel32.dll");
    if(kernel && !strcmp(r->last_name,"GetCurrentDirectoryA")) {
        PwGuestCall call={0};uint32_t capacity,destination;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,8,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&capacity))!=PW_OK ||
           (status=pw_guest_call_u32(&call,4,&destination))!=PW_OK)return status;
        const char *directory=r->services.current_directory;
        if(!directory)return PW_ERR_STATE;
        size_t length=0;
        while(length<=PW_PATH_MAX && directory[length])length++;
        if(length==0 || length>PW_PATH_MAX)return PW_ERR_MALFORMED;
        uint32_t required=(uint32_t)length+1u,result;
        unsigned copy=0;
        if(!destination && capacity==0) {
            result=required; /* documented size query */
        } else if(!destination) {
            r->last_error=87; /* ERROR_INVALID_PARAMETER */
            result=0;
        } else if(capacity<required) {
            result=required;
        } else {
            if((status=range_access(state,destination,required,PW_X86_WRITE))!=PW_OK)
                return status;
            result=(uint32_t)length;copy=1;
        }
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,result))!=PW_OK)return status;
        if(copy)memcpy((void *)(uintptr_t)destination,directory,required);
        *state=after;r->calls++;return PW_OK;
    }
    if(kernel && !strcmp(r->last_name,"GetLastError")) {
        PwGuestCall call={0};int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,0,0);
        if(status==PW_OK)status=pw_guest_call_finish(&call,32,r->last_error);
        if(status==PW_OK)r->calls++;
        return status;
    }
    if(kernel && !strcmp(r->last_name,"GetVersion")) {
        PwGuestCall call={0};int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,0,0);
        /* Stable NT 5.1 compatibility profile: major=5, minor=1. */
        if(status==PW_OK)status=pw_guest_call_finish(&call,32,0x00000105u);
        if(status==PW_OK)r->calls++;
        return status;
    }
    if(kernel && !strcmp(r->last_name,"GetModuleFileNameA")) {
        PwGuestCall call={0};uint32_t module,destination,capacity;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,12,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&module))!=PW_OK ||
           (status=pw_guest_call_u32(&call,4,&destination))!=PW_OK ||
           (status=pw_guest_call_u32(&call,8,&capacity))!=PW_OK)return status;
        if(module && module!=r->main_base)return PW_ERR_UNSUPPORTED;
        const char *filename=r->services.main_module_filename;
        if(!filename)return PW_ERR_STATE;
        size_t length=0;
        while(length<=PW_PATH_MAX && filename[length])length++;
        if(length>PW_PATH_MAX)return PW_ERR_LIMIT;
        size_t written=length<capacity?length+1:capacity;
        if(written && (status=range_access(state,destination,written,PW_X86_WRITE))!=PW_OK)
            return status;
        PwX86State after=*state;call.state=&after;
        uint32_t result=length<capacity?(uint32_t)length:capacity;
        if((status=pw_guest_call_finish(&call,32,result))!=PW_OK)return status;
        if(written)memcpy((void *)(uintptr_t)destination,filename,written);
        *state=after;r->calls++;return PW_OK;
    }
    if(kernel && !strcmp(r->last_name,"GetPrivateProfileIntA")) {
        if(!r->services.profile_int)return PW_ERR_STATE;
        PwGuestCall call={0};uint32_t arg[4];
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,16,0);
        if(status!=PW_OK)return status;
        for(unsigned i=0;i<4;i++)
            if((status=pw_guest_call_u32(&call,i*4,&arg[i]))!=PW_OK)return status;
        char section[128],key[128],filename[PW_PATH_MAX+1];
        if((status=guest_string(state,arg[0],section,sizeof(section),0))!=PW_OK ||
           (status=guest_string(state,arg[1],key,sizeof(key),0))!=PW_OK ||
           (status=guest_string(state,arg[3],filename,sizeof(filename),0))!=PW_OK)
            return status;
        uint32_t result=arg[2];
        status=r->services.profile_int(r->services.opaque,section,key,arg[2],filename,&result);
        if(status!=PW_OK)return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,result))!=PW_OK)return status;
        *state=after;r->calls++;return PW_OK;
    }
    if(kernel && (!strcmp(r->last_name,"LocalAlloc") || !strcmp(r->last_name,"LocalFree") ||
                  !strcmp(r->last_name,"GlobalAlloc") || !strcmp(r->last_name,"GlobalFree") ||
                  !strcmp(r->last_name,"GlobalLock") || !strcmp(r->last_name,"GlobalUnlock") ||
                  !strcmp(r->last_name,"GlobalHandle"))) {
        if(!r->heap)return PW_ERR_STATE;
        int status=pw_guest_heap_validate(r->heap);if(status!=PW_OK)return status;
        uint64_t heap_end=(uint64_t)r->heap->base+r->heap->bytes;
        if(r->heap->base<state->stack_high && heap_end>state->stack_low)return PW_ERR_STATE;
        if((status=range_access(state,r->heap->base,r->heap->bytes,
                                PW_X86_READ|PW_X86_WRITE))!=PW_OK)return status;
        unsigned allocate=!strcmp(r->last_name,"LocalAlloc") || !strcmp(r->last_name,"GlobalAlloc");
        unsigned release=!strcmp(r->last_name,"LocalFree") || !strcmp(r->last_name,"GlobalFree");
        unsigned one_arg=!allocate;PwGuestCall call={0};uint32_t flags=0,value=0,result=0;
        if((status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,(one_arg?1:2)*4,0))!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,allocate?&flags:&value))!=PW_OK)return status;
        if(allocate && (status=pw_guest_call_u32(&call,4,&value))!=PW_OK)return status;
        /* GMEM_MOVEABLE is represented by a stable identity handle in this
         * single-process guest heap; DISCARDABLE/SHARE remain accepted hints. */
        if(allocate && (flags&~0x2042u))return PW_ERR_UNSUPPORTED;
        uint32_t requested=0;
        if(!allocate && value && (status=pw_guest_heap_query(r->heap,value,&requested))!=PW_OK)
            return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,release?value:0))!=PW_OK)return status;
        if(allocate) {
            status=(flags&0x40u)?pw_guest_heap_calloc(r->heap,1,value,&result):
                pw_guest_heap_alloc(r->heap,value,&result);
            if(status==PW_ERR_LIMIT){status=PW_OK;result=0;r->last_error=8;}
        } else if(release) {
            status=pw_guest_heap_free(r->heap,value);if(status==PW_OK)result=0;
        } else if(!strcmp(r->last_name,"GlobalUnlock"))result=0;
        else result=value; /* fixed GlobalLock/GlobalHandle identity */
        if(status!=PW_OK)return status;
        after.gpr[0]=result;*state=after;r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"msvcrt.dll") &&
       (!strcmp(r->last_name,"isalnum") || !strcmp(r->last_name,"isdigit") ||
        !strcmp(r->last_name,"isspace"))) {
        PwGuestCall call={0};uint32_t character;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_CDECL,4,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&character))!=PW_OK)return status;
        unsigned byte=character&255u,result=0;
        if(character<=255) {
            if(!strcmp(r->last_name,"isdigit"))result=byte>='0' && byte<='9';
            else if(!strcmp(r->last_name,"isspace"))
                result=byte==' ' || (byte>='\t' && byte<='\r');
            else result=(byte>='0' && byte<='9') || (byte>='A' && byte<='Z') ||
                (byte>='a' && byte<='z');
        }
        if((status=pw_guest_call_finish(&call,32,result))!=PW_OK)return status;
        r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"msvcrt.dll") &&
       (!strcmp(r->last_name,"atoi") || !strcmp(r->last_name,"atol"))) {
        PwGuestCall call={0};uint32_t address,length;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_CDECL,4,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&address))!=PW_OK ||
           (status=string_length(state,address,&length))!=PW_OK)return status;
        const uint8_t *p=(const uint8_t *)(uintptr_t)address,*end=p+length;
        while(p<end && (*p==' ' || (*p>='\t' && *p<='\r')))p++;
        unsigned negative=0;if(p<end && (*p=='+' || *p=='-'))negative=*p++=='-';
        uint32_t value=0;while(p<end && *p>='0' && *p<='9')value=value*10u+(*p++-'0');
        uint32_t result=negative?0u-value:value;
        if((status=pw_guest_call_finish(&call,32,result))!=PW_OK)return status;
        r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"msvcrt.dll") &&
       (!strcmp(r->last_name,"_itoa") || !strcmp(r->last_name,"_ltoa"))) {
        PwGuestCall call={0};uint32_t input,destination,radix;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_CDECL,12,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&input))!=PW_OK ||
           (status=pw_guest_call_u32(&call,4,&destination))!=PW_OK ||
           (status=pw_guest_call_u32(&call,8,&radix))!=PW_OK)return status;
        if(radix<2 || radix>36)return PW_ERR_UNSUPPORTED;
        char reversed[34],output[35];unsigned count=0,negative=radix==10 && (int32_t)input<0;
        uint32_t magnitude=negative?(uint32_t)(0u-input):input;
        do {
            unsigned digit=magnitude%radix;magnitude/=radix;
            reversed[count++]=(char)(digit<10?'0'+digit:'a'+digit-10);
        } while(magnitude);
        unsigned length=0;if(negative)output[length++]='-';
        while(count)output[length++]=reversed[--count];
        output[length]=0;
        if((status=range_access(state,destination,(size_t)length+1,PW_X86_WRITE))!=PW_OK)return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,destination))!=PW_OK)return status;
        memcpy((void *)(uintptr_t)destination,output,(size_t)length+1);
        *state=after;r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"msvcrt.dll") && !strcmp(r->last_name,"memmove")) {
        PwGuestCall call={0};uint32_t destination,source,count;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_CDECL,12,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&destination))!=PW_OK ||
           (status=pw_guest_call_u32(&call,4,&source))!=PW_OK ||
           (status=pw_guest_call_u32(&call,8,&count))!=PW_OK)return status;
        if(count && ((status=range_access(state,source,count,PW_X86_READ))!=PW_OK ||
                     (status=range_access(state,destination,count,PW_X86_WRITE))!=PW_OK))return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,destination))!=PW_OK)return status;
        if(count)memmove((void *)(uintptr_t)destination,(const void *)(uintptr_t)source,count);
        *state=after;r->calls++;return PW_OK;
    }
    if((!strcmp(r->last_dll,"msvcrt.dll") && !strcmp(r->last_name,"sprintf")) ||
       (!strcmp(r->last_dll,"user32.dll") && !strcmp(r->last_name,"wsprintfA"))) {
        PwGuestCall call={0};uint32_t destination,format_address;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_CDECL,8,1);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&destination))!=PW_OK ||
           (status=pw_guest_call_u32(&call,4,&format_address))!=PW_OK)return status;
        char format[1024],output[4096];
        if((status=guest_string(state,format_address,format,sizeof(format),0))!=PW_OK)return status;
        size_t capacity;
        if((status=writable_capacity(state,destination,sizeof(output),&capacity))!=PW_OK)return status;
        FormatGuest guest={&call,state};PwCrtFormatInput input={&guest,format_u32,format_string};
        uint32_t length;
        if((status=pw_crt_format_ascii(output,capacity,format,&input,&length))!=PW_OK)return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,length))!=PW_OK)return status;
        memcpy((void *)(uintptr_t)destination,output,(size_t)length+1);
        *state=after;r->calls++;return PW_OK;
    }
    unsigned compare_string=kernel && !strcmp(r->last_name,"lstrcmpA");
    unsigned search_string=!strcmp(r->last_dll,"msvcrt.dll") && !strcmp(r->last_name,"strstr");
    if(compare_string || search_string) {
        PwGuestCall call={0};uint32_t first,second,first_length,second_length;
        int status=pw_guest_call_begin(&call,state,
            compare_string?PW_GUEST_STDCALL:PW_GUEST_CDECL,8,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&first))!=PW_OK ||
           (status=pw_guest_call_u32(&call,4,&second))!=PW_OK ||
           (status=string_length(state,first,&first_length))!=PW_OK ||
           (status=string_length(state,second,&second_length))!=PW_OK)return status;
        const uint8_t *a=(const uint8_t *)(uintptr_t)first;
        const uint8_t *b=(const uint8_t *)(uintptr_t)second;uint32_t result=0;
        if(compare_string) {
            uint32_t common=first_length<second_length?first_length:second_length;int order=0;
            for(uint32_t i=0;i<common && !order;i++)order=a[i]<b[i]?-1:a[i]>b[i]?1:0;
            if(!order)order=first_length<second_length?-1:first_length>second_length?1:0;
            result=(uint32_t)order;
        } else if(!second_length)result=first;
        else if(second_length<=first_length) {
            for(uint32_t i=0;i<=first_length-second_length;i++)
                if(!memcmp(a+i,b,second_length)){result=first+i;break;}
        }
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,result))!=PW_OK)return status;
        *state=after;r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"msvcrt.dll") && !strcmp(r->last_name,"_strnicmp")) {
        PwGuestCall call={0};uint32_t first,second,count;int32_t result=0;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_CDECL,12,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&first))!=PW_OK ||
           (status=pw_guest_call_u32(&call,4,&second))!=PW_OK ||
           (status=pw_guest_call_u32(&call,8,&count))!=PW_OK ||
           (status=compare_ascii_nocase(state,first,second,count,&result))!=PW_OK)
            return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,(uint32_t)result))!=PW_OK)return status;
        *state=after;r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"msvcrt.dll") &&
       (!strcmp(r->last_name,"fopen") || !strcmp(r->last_name,"fclose"))) {
        unsigned opening=!strcmp(r->last_name,"fopen");
        if((opening && !r->services.file_open) || (!opening && !r->services.file_close))
            return PW_ERR_STATE;
        PwGuestCall call={0};uint32_t a[2]={0};
        int status=pw_guest_call_begin(&call,state,PW_GUEST_CDECL,opening?8:4,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&a[0]))!=PW_OK ||
           (opening && (status=pw_guest_call_u32(&call,4,&a[1]))!=PW_OK))return status;
        uint32_t result=0;
        if(opening) {
            char path[PW_PATH_MAX+1],mode[8];
            if((status=guest_string(state,a[0],path,sizeof(path),0))!=PW_OK ||
               (status=guest_string(state,a[1],mode,sizeof(mode),0))!=PW_OK)return status;
            status=r->services.file_open(r->services.opaque,path,mode,&result);
            if(status==PW_ERR_NOT_FOUND){status=PW_OK;result=0;r->crt_errno=2;}
        } else {
            status=r->services.file_close(r->services.opaque,a[0]);
            result=status==PW_OK?0:UINT32_MAX;
            if(status==PW_ERR_NOT_FOUND){status=PW_OK;r->crt_errno=9;}
        }
        if(status!=PW_OK)return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,result))!=PW_OK)return status;
        *state=after;r->calls++;return PW_OK;
    }
    if(kernel && (!strcmp(r->last_name,"OpenFile") || !strcmp(r->last_name,"_lopen") ||
                  !strcmp(r->last_name,"_lread") || !strcmp(r->last_name,"_hread") ||
                  !strcmp(r->last_name,"_llseek") || !strcmp(r->last_name,"_lclose"))) {
        unsigned opening=!strcmp(r->last_name,"OpenFile") || !strcmp(r->last_name,"_lopen");
        unsigned open_file=!strcmp(r->last_name,"OpenFile");
        unsigned reading=!strcmp(r->last_name,"_lread") || !strcmp(r->last_name,"_hread");
        unsigned seeking=!strcmp(r->last_name,"_llseek");
        unsigned count=(open_file || reading || seeking)?3:opening?2:1;
        if((opening && !r->services.file_open) || (reading && !r->services.file_read) ||
           (seeking && !r->services.file_seek) || (!opening && !reading && !seeking && !r->services.file_close))
            return PW_ERR_STATE;
        PwGuestCall call={0};uint32_t a[3]={0};
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,count*4,0);
        if(status!=PW_OK)return status;
        for(unsigned i=0;i<count;i++)if((status=pw_guest_call_u32(&call,i*4,&a[i]))!=PW_OK)return status;
        char path[PW_PATH_MAX+1];uint32_t result=UINT32_MAX;
        if(opening) {
            if((status=guest_string(state,a[0],path,sizeof(path),0))!=PW_OK)return status;
            if((open_file && a[2]) &&
               (status=range_access(state,a[1],136,PW_X86_WRITE))!=PW_OK)return status;
        } else if(reading && a[2] &&
                  (status=range_access(state,a[1],a[2],PW_X86_WRITE))!=PW_OK)return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,0))!=PW_OK)return status;
        if(opening) {
            if((open_file?a[2]:a[1])!=0)status=PW_ERR_UNSUPPORTED; /* OF_READ only */
            else status=r->services.file_open(r->services.opaque,path,"rb",&result);
            if(status==PW_ERR_NOT_FOUND){status=PW_OK;result=UINT32_MAX;}
            if(status==PW_OK && result!=UINT32_MAX && open_file && a[1]) {
                uint8_t ofstruct[136]={0};ofstruct[0]=136;
                size_t n=strlen(path);if(n>127)n=127;memcpy(ofstruct+8,path,n);
                memcpy((void *)(uintptr_t)a[1],ofstruct,sizeof(ofstruct));
            }
        } else if(reading) {
            uint32_t got=0;status=r->services.file_read(r->services.opaque,a[0],
                (void *)(uintptr_t)a[1],a[2],&got);if(status==PW_OK)result=got;
        } else if(seeking)
            status=r->services.file_seek(r->services.opaque,a[0],(int32_t)a[1],a[2],&result);
        else {status=r->services.file_close(r->services.opaque,a[0]);result=status==PW_OK?0:UINT32_MAX;}
        if(status==PW_ERR_NOT_FOUND || status==PW_ERR_STATE || status==PW_ERR_PRECONDITION)
            {status=PW_OK;result=UINT32_MAX;}
        if(status!=PW_OK)return status;
        after.gpr[0]=result;*state=after;r->calls++;return PW_OK;
    }
    if(kernel && (!strcmp(r->last_name,"FindResourceA") || !strcmp(r->last_name,"LoadResource") ||
                  !strcmp(r->last_name,"LockResource") || !strcmp(r->last_name,"SizeofResource") ||
                  !strcmp(r->last_name,"FreeResource"))) {
        unsigned find=!strcmp(r->last_name,"FindResourceA");
        unsigned lock=!strcmp(r->last_name,"LockResource");
        unsigned free_resource=!strcmp(r->last_name,"FreeResource");
        unsigned count=find?3:(lock || free_resource)?1:2;PwGuestCall call={0};uint32_t a[3]={0};
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,count*4,0);
        if(status!=PW_OK)return status;
        for(unsigned i=0;i<count;i++)if((status=pw_guest_call_u32(&call,i*4,&a[i]))!=PW_OK)return status;
        uint32_t result=0;const uint8_t *bytes=NULL;size_t size=0;unsigned slot=128;
        if(find) {
            if((a[0] && a[0]!=r->main_base) || a[2]>=0x10000u || !r->heap)return PW_ERR_UNSUPPORTED;
            if(a[1]<0x10000u) {
                if(!r->services.integer_resource)return PW_ERR_STATE;
                status=r->services.integer_resource(r->services.opaque,r->main_base,a[2],a[1],&bytes,&size);
            } else {
                if(!r->services.named_resource)return PW_ERR_STATE;
                char name[PW_USER32_NAME_MAX+1];
                if((status=guest_string(state,a[1],name,sizeof(name),0))!=PW_OK)return status;
                status=r->services.named_resource(r->services.opaque,r->main_base,a[2],name,&bytes,&size);
            }
            if(status==PW_ERR_NOT_FOUND){status=PW_OK;result=0;r->last_error=1813;}
            else if(status==PW_OK) {
                if(!bytes || !size || size>UINT32_MAX)return PW_ERR_MALFORMED;
                for(unsigned i=0;i<128;i++)if(!r->resources[i].used){slot=i;break;}
                if(slot==128)return PW_ERR_LIMIT;
            }
        } else if(!free_resource) {
            uint32_t handle=lock?a[0]:a[1];
            for(unsigned i=0;i<128;i++)if(r->resources[i].used && r->resources[i].handle==handle){slot=i;break;}
            if(slot==128 || (!lock && a[0] && a[0]!=r->main_base))status=PW_ERR_NOT_FOUND;
            else result=!strcmp(r->last_name,"SizeofResource")?r->resources[slot].size:handle;
            if(status==PW_ERR_NOT_FOUND){status=PW_OK;result=0;r->last_error=6;}
        } else result=0; /* obsolete Win32 API: process owns resource lifetime */
        if(status!=PW_OK)return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,result))!=PW_OK)return status;
        if(find && bytes) {
            status=pw_guest_heap_alloc(r->heap,(uint32_t)size,&result);
            if(status==PW_OK) {
                memcpy((void *)(uintptr_t)result,bytes,size);
                r->resources[slot]=(PwResourceRef){result,(uint32_t)size,1};
            } else if(status==PW_ERR_LIMIT){status=PW_OK;result=0;r->last_error=8;}
            if(status!=PW_OK)return status;
            after.gpr[0]=result;
        }
        *state=after;r->calls++;return PW_OK;
    }
    unsigned bounded=kernel && !strcmp(r->last_name,"lstrcpynA");
    unsigned append=kernel && !strcmp(r->last_name,"lstrcatA");
    if(bounded || append || (kernel && !strcmp(r->last_name,"lstrcpyA"))) {
        PwGuestCall call={0};uint32_t dst,src,count=0,at=0,n=0;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,bounded?12:8,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&dst))!=PW_OK ||
           (status=pw_guest_call_u32(&call,4,&src))!=PW_OK)return status;
        if(bounded && (status=pw_guest_call_u32(&call,8,&count))!=PW_OK)return status;
        if(!bounded || count)status=copy_string(state,dst,src,count,bounded,append,&at,&n);
        unsigned invalid=status==PW_ERR_VM;
        if(status!=PW_OK && !invalid)return status;
        status=pw_guest_call_finish(&call,32,invalid?0:dst);
        if(status!=PW_OK)return status;
        if(invalid)r->last_error=87; /* ERROR_INVALID_PARAMETER, as Wine's bad-pointer path */
        else if(!bounded || count) {
            if(n)memmove((void *)(uintptr_t)at,(const void *)(uintptr_t)src,n);
            *(uint8_t *)(uintptr_t)(at+n)=0;
        }
        r->calls++;return PW_OK;
    }
    if(kernel && !strcmp(r->last_name,"lstrlenA")) {
        PwGuestCall call={0};uint32_t address,length=0;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,4,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&address))!=PW_OK)return status;
        if(address && (status=string_length(state,address,&length))!=PW_OK)return status;
        status=pw_guest_call_finish(&call,32,length);
        if(status==PW_OK)r->calls++;
        return status;
    }
    if(kernel && !strcmp(r->last_name,"GetStartupInfoA")) {
        /* Serialize STARTUPINFOA32, never the host's pointer-sized structure.
         * GUI launch: show policy supplied, no inherited console handles,
         * title/desktop override, geometry override or reserved CRT data. */
        if(r->startup_show>11 || r->startup_show==10)return PW_ERR_UNSUPPORTED;
        PwGuestCall call={0};uint32_t address;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,4,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&address))!=PW_OK)return status;
        if(address>UINT32_MAX-67)return PW_ERR_VM;
        for(unsigned offset=0;offset<68;offset+=4)
            if((status=word_access(state,address+offset,PW_X86_WRITE))!=PW_OK)return status;
        uint8_t info[68]={0};uint32_t size=68,flags=1;
        memcpy(info,&size,4);memcpy(info+44,&flags,4);memcpy(info+48,&r->startup_show,2);
        status=pw_guest_call_finish(&call,0,0);
        if(status!=PW_OK)return status;
        memcpy((void *)(uintptr_t)address,info,sizeof(info));r->calls++;return PW_OK;
    }
    unsigned current_process=kernel && !strcmp(r->last_name,"GetCurrentProcess");
    unsigned current_thread=kernel && !strcmp(r->last_name,"GetCurrentThread");
    if(current_process || current_thread) {
        PwGuestCall call={0};int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,0,0);
        if(status==PW_OK)status=pw_guest_call_finish(&call,32,
            current_process?UINT32_MAX:UINT32_MAX-1); /* documented pseudo handles */
        if(status==PW_OK)r->calls++;
        return status;
    }
    if(kernel && !strcmp(r->last_name,"Sleep")) {
        PwGuestCall call={0};uint32_t milliseconds;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,4,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&milliseconds))!=PW_OK)return status;
        if(!r->services.sleep_ms)return PW_ERR_STATE;
        if((status=r->services.sleep_ms(r->services.opaque,milliseconds))!=PW_OK)return status;
        if((status=pw_guest_call_finish(&call,0,0))!=PW_OK)return status;
        r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"msvcrt.dll") &&
       (!strcmp(r->last_name,"exit") || !strcmp(r->last_name,"_exit"))) {
        PwGuestCall call={0};uint32_t code;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_CDECL,4,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&code))!=PW_OK)return status;
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,0,0))!=PW_OK)return status;
        r->exit_code=code;r->exit_requested=1;*state=after;r->calls++;return PW_OK;
    }
    if(kernel && !strcmp(r->last_name,"SetThreadPriority")) {
        PwGuestCall call={0};uint32_t handle,raw_priority;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,8,0);
        if(status!=PW_OK)return status;
        if((status=pw_guest_call_u32(&call,0,&handle))!=PW_OK ||
           (status=pw_guest_call_u32(&call,4,&raw_priority))!=PW_OK)return status;
        int32_t priority=(int32_t)raw_priority;uint32_t result=1,error=0;
        if(handle!=UINT32_MAX-1 || (priority!=-15 && priority!=-2 && priority!=-1 &&
           priority!=0 && priority!=1 && priority!=2 && priority!=15)) {
            result=0;error=handle!=UINT32_MAX-1?6:87;
        }
        PwX86State after=*state;call.state=&after;
        if((status=pw_guest_call_finish(&call,32,result))!=PW_OK)return status;
        if(result)r->thread_priority=priority;
        *state=after;if(error)r->last_error=error;r->calls++;return PW_OK;
    }
    unsigned wall=kernel && !strcmp(r->last_name,"GetSystemTimeAsFileTime");
    unsigned counter=kernel && !strcmp(r->last_name,"QueryPerformanceCounter");
    unsigned tick=(kernel && !strcmp(r->last_name,"GetTickCount")) ||
        (!strcmp(r->last_dll,"winmm.dll") && !strcmp(r->last_name,"timeGetTime"));
    unsigned pid=kernel && !strcmp(r->last_name,"GetCurrentProcessId");
    unsigned tid=kernel && !strcmp(r->last_name,"GetCurrentThreadId");
    if(wall || counter || tick || pid || tid) {
        PwGuestCall call={0};uint32_t address=0;uint64_t value=0;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,(wall || counter)?4:0,0);
        if(status!=PW_OK)return status;
        if(wall || counter) {
            if((status=pw_guest_call_u32(&call,0,&address))!=PW_OK)return status;
            if(address>UINT32_MAX-7)return PW_ERR_VM;
            if((status=word_access(state,address,PW_X86_WRITE))!=PW_OK)return status;
            if((status=word_access(state,address+4,PW_X86_WRITE))!=PW_OK)return status;
        }
        if(pid || tid) {
            value=pid?r->services.process_id:r->services.thread_id;
            if(!value)return PW_ERR_STATE;
        } else {
            if(!r->services.clock_ns)return PW_ERR_STATE;
            status=r->services.clock_ns(r->services.opaque,wall?PW_CLOCK_UTC:counter?PW_CLOCK_COUNTER:PW_CLOCK_UPTIME,&value);
            if(status!=PW_OK)return status;
            if(wall)value=value/100+116444736000000000ull;
            else if(tick)value=(uint32_t)(value/1000000);
            else if(value>INT64_MAX)return PW_ERR_LIMIT;
        }
        status=pw_guest_call_finish(&call,wall?0:32,counter?1:value);
        if(status!=PW_OK)return status;
        if(wall || counter)memcpy((void *)(uintptr_t)address,&value,8);
        r->calls++;return PW_OK;
    }
    if(!strcmp(r->last_dll,"msvcrt.dll")) {
        if(!strcmp(r->last_name,"rand")) {
            PwGuestCall call={0};int status=pw_guest_call_begin(&call,state,PW_GUEST_CDECL,0,0);
            if(status!=PW_OK)return status;
            uint32_t next=r->rand_state*214013u+2531011u;
            PwX86State after=*state;call.state=&after;
            if((status=pw_guest_call_finish(&call,32,(next>>16)&0x7fffu))!=PW_OK)return status;
            r->rand_state=next;*state=after;r->calls++;return PW_OK;
        }
        if(!strcmp(r->last_name,"_CIacos")) {
            PwGuestCall call={0};int status=pw_guest_call_begin(&call,state,PW_GUEST_CDECL,0,0);
            if(status!=PW_OK)return status;
            PwX86State after=*state;call.state=&after;
            if((status=pw_guest_call_finish(&call,32,0))!=PW_OK)return status;
            if((status=pw_x87_execute(&after.fp,PW_X87_FACOS,0,NULL))!=PW_OK)return status;
            *state=after;r->calls++;return PW_OK;
        }
        if(!strcmp(r->last_name,"_ftol")) {
            PwGuestCall call={0};int status=pw_guest_call_begin(&call,state,PW_GUEST_CDECL,0,0);
            if(status!=PW_OK)return status;
            uint8_t raw[10];if((status=pw_guest_x87_peek(&state->fp,0,raw))!=PW_OK)return status;
            uint64_t significand;uint16_t sign_exponent;
            memcpy(&significand,raw,8);memcpy(&sign_exponent,raw+8,2);
            unsigned negative=sign_exponent>>15,field=sign_exponent&0x7fffu;
            int64_t value=0;int exponent=(int)field-16383;
            if(field==0x7fff || exponent>63 || (exponent==63 && (!negative || significand!=UINT64_C(0x8000000000000000))))
                value=INT64_MIN;
            else if(exponent>=0) {
                uint64_t magnitude=significand>>(63-exponent);
                value=negative?(magnitude==UINT64_C(0x8000000000000000)?INT64_MIN:-(int64_t)magnitude):(int64_t)magnitude;
            }
            PwX86State after=*state;call.state=&after;
            if((status=pw_guest_call_finish(&call,32,(uint32_t)value))!=PW_OK)return status;
            if((status=pw_guest_x87_pop(&after.fp,raw))!=PW_OK)return status;
            after.gpr[2]=(uint32_t)((uint64_t)value>>32);*state=after;r->calls++;return PW_OK;
        }
        if(!strcmp(r->last_name,"floor")) {
            PwGuestCall call={0};uint32_t low,high;uint64_t bits;
            int status=pw_guest_call_begin(&call,state,PW_GUEST_CDECL,8,0);
            if(status!=PW_OK)return status;
            if((status=pw_guest_call_u32(&call,0,&low))!=PW_OK ||
               (status=pw_guest_call_u32(&call,4,&high))!=PW_OK)return status;
            bits=(uint64_t)low|(uint64_t)high<<32;bits=floor_binary64(bits);
            PwX86State after=*state;call.state=&after;
            if((status=pw_guest_call_finish(&call,32,0))!=PW_OK)return status;
            status=pw_x87_execute(&after.fp,PW_X87_FLD_F64,(uintptr_t)&bits,NULL);
            if(status!=PW_OK)return status;
            *state=after;r->calls++;return PW_OK;
        }
        unsigned alloc=!strcmp(r->last_name,"malloc") || !strcmp(r->last_name,"??2@YAPAXI@Z");
        unsigned zero=!strcmp(r->last_name,"calloc");
        unsigned resize=!strcmp(r->last_name,"realloc");
        unsigned release=!strcmp(r->last_name,"free") ||
            !strcmp(r->last_name,"??3@YAXPAX@Z");
        if(alloc || zero || resize || release) {
            int status=pw_guest_heap_validate(r->heap);
            if(status!=PW_OK)return status;
            uint64_t end=(uint64_t)r->heap->base+r->heap->bytes;
            if(r->heap->base<state->stack_high && end>state->stack_low)return PW_ERR_STATE;
            if((status=range_access(state,r->heap->base,r->heap->bytes,PW_X86_READ|PW_X86_WRITE))!=PW_OK)return status;
            PwGuestCall call={0};uint32_t first,second=0,address=0;
            status=pw_guest_call_begin(&call,state,PW_GUEST_CDECL,(zero || resize)?8:4,0);
            if(status!=PW_OK)return status;
            if((status=pw_guest_call_u32(&call,0,&first))!=PW_OK)return status;
            if((zero || resize) && (status=pw_guest_call_u32(&call,4,&second))!=PW_OK)return status;
            /* Preflight return before allocator side effects. The synchronous
             * heap core never mutates guest registers or calls guest code. */
            PwX86State after=*state;call.state=&after;
            if((status=pw_guest_call_finish(&call,release?0:32,0))!=PW_OK)return status;
            if(alloc)status=pw_guest_heap_alloc(r->heap,first,&address);
            else if(zero)status=pw_guest_heap_calloc(r->heap,first,second,&address);
            else if(resize)status=pw_guest_heap_realloc(r->heap,first,second,&address);
            else status=pw_guest_heap_free(r->heap,first);
            if(status==PW_ERR_LIMIT && !release) {
                /* Default CRT new-handler is absent. No setter is currently
                 * implemented; new_mode alone cannot create a handler. */
                r->crt_errno=12;address=0; /* guest ENOMEM, not host errno */
            } else if(status!=PW_OK)return status;
            if(!release)after.gpr[0]=address;
            *state=after;r->calls++;return PW_OK;
        }
        if(!strcmp(r->last_name,"__getmainargs")) {
            PwGuestCall call={0};uint32_t a[5],mode=r->new_mode;
            int status=pw_guest_call_begin(&call,state,PW_GUEST_CDECL,20,0);
            if(status!=PW_OK)return status;
            for(unsigned i=0;i<5;i++)if((status=pw_guest_call_u32(&call,i*4,&a[i]))!=PW_OK)return status;
            if(a[3])return PW_ERR_UNSUPPORTED;
            for(unsigned i=0;i<3;i++)if((status=word_access(state,a[i],PW_X86_WRITE))!=PW_OK)return status;
            if(a[4] && (status=table_read(state,a[4],&mode))!=PW_OK)return status;
            if(mode>1)return PW_ERR_UNSUPPORTED;
            status=pw_guest_call_finish(&call,32,0);
            if(status!=PW_OK)return status;
            memcpy((void *)(uintptr_t)a[0],&r->args.argc,4);
            memcpy((void *)(uintptr_t)a[1],&r->args.argv,4);
            memcpy((void *)(uintptr_t)a[2],&r->args.envp,4);
            r->new_mode=mode;r->calls++;return PW_OK;
        }
        if(!strcmp(r->last_name,"_initterm")) {
            if(r->init_depth==PW_WIN32_INIT_DEPTH)return PW_ERR_LIMIT;
            PwWin32Init next={0};
            int status=pw_guest_call_begin(&next.call,state,PW_GUEST_CDECL,8,0);
            if(status!=PW_OK)return status;
            if((status=pw_guest_call_u32(&next.call,0,&next.cursor))!=PW_OK)return status;
            if((status=pw_guest_call_u32(&next.call,4,&next.end))!=PW_OK)return status;
            if((next.cursor&3) || (next.end&3) || next.end<next.cursor)return PW_ERR_UNSUPPORTED;
            if((next.end-next.cursor)/4>PW_WIN32_INIT_ENTRIES)return PW_ERR_LIMIT;
            r->init[r->init_depth++]=next;
            status=init_next(r,state);
            if(status!=PW_OK){memset(&r->init[--r->init_depth],0,sizeof(next));}
            return status;
        }
        if(!strcmp(r->last_name,"_controlfp")) {
            PwGuestCall call={0};uint32_t value,mask,result;
            int status=pw_guest_call_begin(&call,state,PW_GUEST_CDECL,8,0);
            if(status!=PW_OK)return status;
            if((status=pw_guest_call_u32(&call,0,&value))!=PW_OK)return status;
            if((status=pw_guest_call_u32(&call,4,&mask))!=PW_OK)return status;
            PwGuestFp next=state->fp;
            if((status=pw_guest_fp_control(&next,value,mask,&result))!=PW_OK)return status;
            status=pw_guest_call_finish(&call,32,result);
            if(status==PW_OK){state->fp=next;r->calls++;}
            return status;
        }
        unsigned set_type=!strcmp(r->last_name,"__set_app_type");
        unsigned fmode=!strcmp(r->last_name,"__p__fmode");
        unsigned commode=!strcmp(r->last_name,"__p__commode");
        if(!set_type && !fmode && !commode)return PW_ERR_UNSUPPORTED;
        PwGuestCall call={0};uint32_t type=0;
        int status=pw_guest_call_begin(&call,state,PW_GUEST_CDECL,set_type?4:0,0);
        if(status!=PW_OK)return status;
        if(set_type) {
            status=pw_guest_call_u32(&call,0,&type);
            if(status!=PW_OK)return status;
        }
        status=pw_guest_call_finish(&call,set_type?0:32,r->crt_data+(fmode?8:12));
        if(status==PW_OK) {
            if(set_type)r->app_type=type;
            r->calls++;
        }
        return status;
    }
    if(strcmp(r->last_dll,"kernel32.dll") || strcmp(r->last_name,"GetModuleHandleA"))
        return PW_ERR_UNSUPPORTED;
    PwGuestCall call={0};uint32_t name;
    int status=pw_guest_call_begin(&call,state,PW_GUEST_STDCALL,4,0);
    if(status!=PW_OK)return status;
    status=pw_guest_call_u32(&call,0,&name);
    if(status!=PW_OK)return status;
    if(name)return PW_ERR_UNSUPPORTED;
    status=pw_guest_call_finish(&call,32,r->main_base);
    if(status==PW_OK)r->calls++;
    return status;
}
