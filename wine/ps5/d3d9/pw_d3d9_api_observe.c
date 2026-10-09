/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_api_observe.h"
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
static INIT_ONCE enabled_once=INIT_ONCE_STATIC_INIT;
static int enabled;
static BOOL CALLBACK initialize(INIT_ONCE *once,void *parameter,void **context)
{
 char value[2];(void)once;(void)parameter;(void)context;
 enabled=GetEnvironmentVariableA("PW_D3D9_DIAGNOSTICS",value,sizeof(value))==1&&value[0]=='1';return TRUE;
}
int pw_d3d9_api_observe_enabled(void)
{InitOnceExecuteOnce(&enabled_once,initialize,NULL,NULL);return enabled;}
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
