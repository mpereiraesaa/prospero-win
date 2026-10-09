/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_private_data.h"
#include <stdint.h>
#include <string.h>
struct pw_d3d9_private_entry {
 struct pw_d3d9_private_entry *next;
 GUID key;
 LONG references;
 DWORD length,allocation;
 unsigned is_interface;
 IUnknown *unknown;
 unsigned char bytes[];
};
static LONG allocated;
static int reserve(DWORD n)
{
 LONG old=InterlockedCompareExchange(&allocated,0,0),seen;
 do{
  if(n>PW_D3D9_PRIVATE_MAX_BYTES-(DWORD)old)return 0;
  seen=InterlockedCompareExchange(&allocated,old+(LONG)n,old);
  if(seen==old)return 1;
  old=seen;
 }while(1);
}
static void drop(struct pw_d3d9_private_entry *e)
{
 if(!e||InterlockedDecrement(&e->references))return;
 if(e->unknown)IUnknown_Release(e->unknown);
 DWORD n=e->allocation;HeapFree(GetProcessHeap(),0,e);InterlockedExchangeAdd(&allocated,-(LONG)n);
}
static struct pw_d3d9_private_entry **find(struct pw_d3d9_private_data *s,REFGUID key)
{
 struct pw_d3d9_private_entry **p=&s->entries;
 while(*p&&!IsEqualGUID(&(*p)->key,key))p=&(*p)->next;
 return p;
}
HRESULT pw_d3d9_private_free(struct pw_d3d9_private_data *s,REFGUID key)
{
 if(!s||!key)return D3DERR_INVALIDCALL;
 AcquireSRWLockExclusive(&s->lock);
 struct pw_d3d9_private_entry **link=find(s,key),*old=*link;
 if(old){*link=old->next;s->count--;}
 ReleaseSRWLockExclusive(&s->lock);drop(old);return S_OK;
}
HRESULT pw_d3d9_private_set(struct pw_d3d9_private_data *s,REFGUID key,const void *data,DWORD length,DWORD flags)
{
 if(!s||!key)return D3DERR_INVALIDCALL;
 unsigned iface=!!(flags&D3DSPD_IUNKNOWN);
 if(iface&&length!=sizeof(IUnknown *))return D3DERR_INVALIDCALL;
 if(!iface&&!data)return pw_d3d9_private_free(s,key);
 DWORD payload=iface?0:length;
 if(payload>PW_D3D9_PRIVATE_MAX_BYTES-sizeof(struct pw_d3d9_private_entry))return E_OUTOFMEMORY;
 DWORD allocation=(DWORD)sizeof(struct pw_d3d9_private_entry)+payload;
 if(!reserve(allocation))return E_OUTOFMEMORY;
 struct pw_d3d9_private_entry *e=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,allocation),*old=NULL;
 if(!e){InterlockedExchangeAdd(&allocated,-(LONG)allocation);return E_OUTOFMEMORY;}
 e->key=*key;e->references=1;e->length=length;e->allocation=allocation;e->is_interface=iface;
 if(iface){e->unknown=(IUnknown *)data;if(e->unknown)IUnknown_AddRef(e->unknown);}
 else if(length)memcpy(e->bytes,data,length);
 HRESULT hr=S_OK;AcquireSRWLockExclusive(&s->lock);
 struct pw_d3d9_private_entry **link=find(s,key);
 if(s->closed)hr=D3DERR_INVALIDCALL;
 else if(!*link&&s->count==PW_D3D9_PRIVATE_MAX_ENTRIES)hr=E_OUTOFMEMORY;
 else {old=*link;e->next=old?old->next:NULL;*link=e;if(!old)s->count++;}
 ReleaseSRWLockExclusive(&s->lock);
 if(FAILED(hr))drop(e);else drop(old);return hr;
}
HRESULT pw_d3d9_private_get(struct pw_d3d9_private_data *s,REFGUID key,void *data,DWORD *length)
{
 if(!data&&!length)return D3DERR_NOTFOUND;
 if(!s||!key||!length)return D3DERR_INVALIDCALL;
 AcquireSRWLockShared(&s->lock);struct pw_d3d9_private_entry *e=*find(s,key);
 if(e)InterlockedIncrement(&e->references);
 ReleaseSRWLockShared(&s->lock);
 if(!e){*length=0;return D3DERR_NOTFOUND;}
 DWORD capacity=*length;*length=e->length;HRESULT hr=S_OK;
 if(data){
  if(capacity<e->length)hr=D3DERR_MOREDATA;
  else if(e->is_interface){if(e->unknown)IUnknown_AddRef(e->unknown);memcpy(data,&e->unknown,sizeof(e->unknown));}
  else if(e->length)memcpy(data,e->bytes,e->length);
 }
 drop(e);return hr;
}
void pw_d3d9_private_dispose(struct pw_d3d9_private_data *s)
{
 if(!s)return;
 AcquireSRWLockExclusive(&s->lock);struct pw_d3d9_private_entry *list=s->entries;
 s->entries=NULL;s->count=0;s->closed=1;ReleaseSRWLockExclusive(&s->lock);
 while(list){struct pw_d3d9_private_entry *next=list->next;drop(list);list=next;}
}
