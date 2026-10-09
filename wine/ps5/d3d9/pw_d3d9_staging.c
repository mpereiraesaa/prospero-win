/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_staging.h"
#include "../pw_d3d9_resource_wire.h"
#include <windows.h>
static LONG staging_bytes;
uint32_t pw_d3d9_staging_bytes(void)
{return (uint32_t)InterlockedCompareExchange(&staging_bytes,0,0);}
static int reserve(uint32_t n)
{
 LONG old=InterlockedCompareExchange(&staging_bytes,0,0),seen;
 do{
  if(n>PW_D3D9_RESOURCE_MAX_LOCK-(uint32_t)old)return 0;
  seen=InterlockedCompareExchange(&staging_bytes,old+(LONG)n,old);
  if(seen==old)return 1;
  old=seen;
 }while(1);
}
uint32_t pw_d3d9_staging_free(void *data,uint32_t n)
{
 if(!data||!n||n>PW_D3D9_RESOURCE_MAX_LOCK)return E_INVALIDARG;
 if(!VirtualFree(data,0,MEM_RELEASE))return E_FAIL;
 InterlockedExchangeAdd(&staging_bytes,-(LONG)n);return S_OK;
}
uint32_t pw_d3d9_staging_alloc(uint32_t n,void **out)
{
 void *data;if(!out)return E_INVALIDARG;*out=NULL;
 if(!n||n>PW_D3D9_RESOURCE_MAX_LOCK)return E_INVALIDARG;
 if(!reserve(n))return E_OUTOFMEMORY;
 data=VirtualAlloc(NULL,n,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
 if(!data){InterlockedExchangeAdd(&staging_bytes,-(LONG)n);return E_OUTOFMEMORY;}
 if((uintptr_t)data>UINT32_MAX-(n-1u)){
  uint32_t hr=pw_d3d9_staging_free(data,n);return hr==S_OK?(uint32_t)E_OUTOFMEMORY:hr;
 }
 *out=data;return S_OK;
}
