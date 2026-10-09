/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_api_observe.h"
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
static INIT_ONCE enabled_once=INIT_ONCE_STATIC_INIT;
static int enabled;
static BOOL CALLBACK initialize(INIT_ONCE *once,void *parameter,void **context)
{
 char value[2];(void)once;(void)parameter;(void)context;
 enabled=GetEnvironmentVariableA("PW_D3D9_DIAGNOSTICS",value,sizeof(value))==1&&value[0]=='1';return TRUE;
}
int pw_d3d9_api_observe_enabled(void)
{InitOnceExecuteOnce(&enabled_once,initialize,NULL,NULL);return enabled;}
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
