/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_api_observe.h"
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
static INIT_ONCE enabled_once=INIT_ONCE_STATIC_INIT;
static int enabled,diagnostics,profile,classification_valid;
static uintptr_t module_begin,module_end;
static DECLSPEC_ALIGN(8) LONG64 entries[PW_D3D9_API_INTERFACES][PW_D3D9_API_SLOTS];
struct pw_d3d9_api_method { const char *interface_name,*method;unsigned interface_id,slot; };
static BOOL CALLBACK initialize(INIT_ONCE *once,void *parameter,void **context)
{
 char value[2];(void)once;(void)parameter;(void)context;
 diagnostics=GetEnvironmentVariableA("PW_D3D9_DIAGNOSTICS",value,sizeof(value))==1&&value[0]=='1';
 profile=GetEnvironmentVariableA("PW_D3D9_PROFILE",value,sizeof(value))==1&&value[0]=='1';
 enabled=diagnostics||profile;
 if(profile){
  HMODULE module=NULL;
  if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
    (LPCSTR)&enabled,&module)){
   const IMAGE_DOS_HEADER *dos=(const void *)module;
   if(dos->e_magic==IMAGE_DOS_SIGNATURE&&dos->e_lfanew>0&&dos->e_lfanew<0x100000){
    const IMAGE_NT_HEADERS *nt=(const void *)((const char *)module+dos->e_lfanew);
    uintptr_t begin=(uintptr_t)module,size=nt->OptionalHeader.SizeOfImage;
    if(nt->Signature==IMAGE_NT_SIGNATURE&&size>(uintptr_t)dos->e_lfanew+sizeof(*nt)&&size<=UINTPTR_MAX-begin){
     module_begin=begin;module_end=begin+size;classification_valid=1;
    }
   }
  }
 }
 return TRUE;
}
int pw_d3d9_api_observe_enabled(void)
{InitOnceExecuteOnce(&enabled_once,initialize,NULL,NULL);return enabled;}
int pw_d3d9_api_diagnostics_enabled(void){return diagnostics;}
void pw_d3d9_api_profile_enter(unsigned interface_id,unsigned slot,const void *caller)
{
 uintptr_t pc=(uintptr_t)caller;
 if(profile&&classification_valid&&pc&&(pc<module_begin||pc>=module_end)
   &&interface_id<PW_D3D9_API_INTERFACES&&slot<PW_D3D9_API_SLOTS)
  InterlockedIncrement64(&entries[interface_id][slot]);
}
void pw_d3d9_api_profile_snapshot(struct pw_d3d9_api_profile_snapshot *out)
{
 out->enabled=profile;out->classification_valid=classification_valid;
 for(unsigned i=0;i<PW_D3D9_API_INTERFACES;i++)for(unsigned j=0;j<PW_D3D9_API_SLOTS;j++)
  out->entries[i][j]=(uint64_t)InterlockedCompareExchange64(&entries[i][j],0,0);
}
/* Saturating admission: neither a long session nor concurrent callers can
 * wrap the counter and reopen output sampling. One counter per typed method. */
int pw_d3d9_api_sample(LONG *counter)
{
 LONG old=InterlockedCompareExchange(counter,0,0);
 while(old<64){LONG found=InterlockedCompareExchange(counter,old+1,old);if(found==old)return 1;old=found;}
 return 0;
}
int pw_d3d9_api_read(void *out,const void *source,SIZE_T bytes)
{
 int saved_errno=errno;DWORD error=GetLastError();SIZE_T copied=0;
 int ok=source&&ReadProcessMemory(GetCurrentProcess(),source,out,bytes,&copied)&&copied==bytes;
 errno=saved_errno;SetLastError(error);return ok;
}
void pw_d3d9_api_output(const char *iface,unsigned slot,const char *method,HRESULT result,void *caller,
 const struct pw_d3d9_api_field *fields,unsigned count)
{
 int saved_errno=errno;DWORD error=GetLastError();char text[4096];
 size_t used=(size_t)snprintf(text,sizeof(text),"PW_D3D9_API_OUTPUT interface=%s slot=%u method=%s hr=%08lx caller=%p",iface,slot,method,(unsigned long)(uint32_t)result,caller);
 for(unsigned i=0;i<count&&used<sizeof(text)-128;i++){
  const struct pw_d3d9_api_field *f=fields+i;
  union {DWORD words[sizeof(D3DCAPS9)/sizeof(DWORD)];void *pointer;D3DLOCKED_RECT locked;} value;
  used+=(size_t)snprintf(text+used,sizeof(text)-used," %s=",f->name);
  if(f->bytes>sizeof(value)||!pw_d3d9_api_read(&value,f->address,f->bytes)){
   used+=(size_t)snprintf(text+used,sizeof(text)-used,"unreadable");continue;
  }
  if(f->kind==PW_D3D9_API_POINTER&&f->bytes==sizeof(value.pointer))
   used+=(size_t)snprintf(text+used,sizeof(text)-used,"%p",value.pointer);
  else if(f->kind==PW_D3D9_API_LOCKED_RECT&&f->bytes==sizeof(value.locked))
   used+=(size_t)snprintf(text+used,sizeof(text)-used,"{Pitch=%ld,pBits=%p}",(long)value.locked.Pitch,value.locked.pBits);
  else for(SIZE_T word=0;word<f->bytes/sizeof(DWORD)&&used<sizeof(text)-16;word++)
   used+=(size_t)snprintf(text+used,sizeof(text)-used,"%s%08lx",word?",":"",(unsigned long)value.words[word]);
 }
 if(used<sizeof(text)-1){text[used++]='\n';text[used]=0;}
#ifdef PW_D3D9_API_OBSERVE_TEST
 extern void pw_d3d9_api_test_output(const char *);
 pw_d3d9_api_test_output(text);
#else
 fprintf(stderr,"%s",text);fflush(stderr);
#endif
 errno=saved_errno;SetLastError(error);
}
void pw_d3d9_api_failure(const char *iface,unsigned slot,const char *method,HRESULT result,void *caller,REFIID iid)
{
 int saved_errno=errno;DWORD error=GetLastError();char text[384],guid[64]="none";GUID copy;SIZE_T copied;
 /* QI can reject a missing/invalid IID. Diagnostics must not turn that failure
  * into an invalid read, including after the target releases its final ref. */
 if(iid){
  if(ReadProcessMemory(GetCurrentProcess(),iid,&copy,sizeof(copy),&copied)&&copied==sizeof(copy))
   snprintf(guid,sizeof(guid),"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",(unsigned long)copy.Data1,copy.Data2,copy.Data3,copy.Data4[0],copy.Data4[1],copy.Data4[2],copy.Data4[3],copy.Data4[4],copy.Data4[5],copy.Data4[6],copy.Data4[7]);
  else snprintf(guid,sizeof(guid),"unreadable");
 }
 snprintf(text,sizeof(text),"PW_D3D9_API_%s interface=%s slot=%u method=%s hr=%08lx caller=%p iid=%s\n",FAILED(result)?"FAIL":"RESULT",iface,slot,method,(unsigned long)(uint32_t)result,caller,guid);
#ifdef PW_D3D9_API_OBSERVE_TEST
 extern void pw_d3d9_api_test_output(const char *);
 pw_d3d9_api_test_output(text);
#else
 fprintf(stderr,"%s",text);fflush(stderr);
#endif
 errno=saved_errno;SetLastError(error);
}
#define PW_D3D9_API_OBSERVE_IMPLEMENTATION
#include "pw_d3d9_api_observe_generated.h"

/* Serialize emitters, never API entry or transport. The first interval includes
 * startup. Atomic per-method reads partition counts without losing entries,
 * but concurrent calls can straddle this multi-counter snapshot. */
static void profile_emit(uint32_t epoch,uint64_t sequence,uint32_t object,uint32_t generation,HRESULT status,int flush)
{
 static SRWLOCK lock=SRWLOCK_INIT;
 static struct pw_d3d9_api_profile_snapshot previous,histogram_previous;
 static uint64_t attempts,histogram_attempt;
 struct pw_d3d9_api_profile_snapshot now;
 uint64_t total=0,delta=0;
 int saved_errno;DWORD error;
 if(!profile)return;
 saved_errno=errno;error=GetLastError();
 AcquireSRWLockExclusive(&lock);
 pw_d3d9_api_profile_snapshot(&now);
 for(unsigned i=0;i<PW_D3D9_API_INTERFACES;i++)for(unsigned j=0;j<PW_D3D9_API_SLOTS;j++){
  total+=now.entries[i][j];delta+=now.entries[i][j]-previous.entries[i][j];
 }
 if(!flush)++attempts;
 fprintf(stderr,"PW_D3D9_API_PROFILE scope=process_external_vtable snapshot=per_method_atomic boundary=%s epoch=%lu sequence=%llu object=%lu generation=%lu hr=%08lx attempt=%llu startup=%u classification_valid=%d api_external_vtable_entries=%llu interval_entries=%llu\n",
  flush?"session_close":"present",(unsigned long)epoch,(unsigned long long)sequence,(unsigned long)object,(unsigned long)generation,(unsigned long)(uint32_t)status,
  (unsigned long long)attempts,attempts==1,classification_valid,(unsigned long long)total,(unsigned long long)delta);
 if(flush||attempts==1||attempts%120==0){
 for(unsigned k=0;k<sizeof(profile_methods)/sizeof(*profile_methods);k++){
  const struct pw_d3d9_api_method *m=profile_methods+k;
  uint64_t n=now.entries[m->interface_id][m->slot],d=n-histogram_previous.entries[m->interface_id][m->slot];
  if(d)fprintf(stderr,"PW_D3D9_API_METHOD scope=process_external_vtable boundary=%s epoch=%lu sequence=%llu interface=%s slot=%u method=%s cumulative=%llu interval=%llu interval_start_attempt=%llu interval_end_attempt=%llu startup=%u\n",
   flush?"session_close":"present",(unsigned long)epoch,(unsigned long long)sequence,m->interface_name,m->slot,m->method,(unsigned long long)n,(unsigned long long)d,(unsigned long long)histogram_attempt,(unsigned long long)attempts,histogram_attempt==0);
 }
 histogram_previous=now;histogram_attempt=attempts;
 }
 previous=now;fflush(stderr);ReleaseSRWLockExclusive(&lock);
 errno=saved_errno;SetLastError(error);
}

void pw_d3d9_api_profile_present(uint32_t epoch,uint64_t sequence,uint32_t object,uint32_t generation,HRESULT status)
{profile_emit(epoch,sequence,object,generation,status,0);}
void pw_d3d9_api_profile_flush(uint32_t epoch)
{profile_emit(epoch,0,0,0,S_OK,1);}
