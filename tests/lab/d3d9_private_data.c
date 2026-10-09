/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "../../wine/ps5/d3d9/pw_d3d9_private_data.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct pw_d3d9_private_data store;
static GUID key={1,2,3,{4}},other={2,2,3,{4}};
static ULONG references=1;
static int reenter;
static HRESULT WINAPI query(IUnknown *p,REFIID i,void **o){(void)p;(void)i;(void)o;return E_NOINTERFACE;}
static ULONG WINAPI add(IUnknown *p)
{
 (void)p;ULONG n=++references;
 if(reenter){reenter=0;assert(SUCCEEDED(pw_d3d9_private_free(&store,&key)));}
 return n;
}
static ULONG WINAPI release(IUnknown *p)
{
 (void)p;DWORD n=0;assert(pw_d3d9_private_get(&store,&other,NULL,&n)==D3DERR_NOTFOUND);return --references;
}
int main(void)
{
 IUnknownVtbl vtable={query,add,release};IUnknown object={&vtable};
 unsigned char source[9]={1,2,3,4,5},out[9]={0};DWORD n;IUnknown *got=NULL;
 assert(pw_d3d9_private_get(&store,&key,NULL,NULL)==D3DERR_NOTFOUND);
 n=9;assert(pw_d3d9_private_get(&store,&key,out,&n)==D3DERR_NOTFOUND&&n==0);
 assert(SUCCEEDED(pw_d3d9_private_set(&store,&key,source,9,0)));source[0]=99;
 n=0;assert(SUCCEEDED(pw_d3d9_private_get(&store,&key,NULL,&n))&&n==9);
 n=8;assert(pw_d3d9_private_get(&store,&key,out,&n)==D3DERR_MOREDATA&&n==9&&!out[0]);
 n=9;assert(SUCCEEDED(pw_d3d9_private_get(&store,&key,out,&n))&&out[0]==1);
 assert(pw_d3d9_private_set(&store,&key,&object,1,D3DSPD_IUNKNOWN)==D3DERR_INVALIDCALL&&references==1);
 assert(SUCCEEDED(pw_d3d9_private_set(&store,&key,&object,sizeof(void *),D3DSPD_IUNKNOWN))&&references==2);
 reenter=1;n=sizeof(got);assert(SUCCEEDED(pw_d3d9_private_get(&store,&key,&got,&n))&&got==&object&&references==2);
 IUnknown_Release(got);assert(references==1);n=4;assert(pw_d3d9_private_get(&store,&key,NULL,&n)==D3DERR_NOTFOUND);
 assert(SUCCEEDED(pw_d3d9_private_set(&store,&key,NULL,sizeof(void *),D3DSPD_IUNKNOWN)));
 got=&object;n=sizeof(got);assert(SUCCEEDED(pw_d3d9_private_get(&store,&key,&got,&n))&&!got);
 assert(SUCCEEDED(pw_d3d9_private_free(&store,&key))&&SUCCEEDED(pw_d3d9_private_free(&store,&key)));
 for(unsigned i=0;i<PW_D3D9_PRIVATE_MAX_ENTRIES;i++){GUID k=key;k.Data1=i+10;assert(SUCCEEDED(pw_d3d9_private_set(&store,&k,source,1,0)));}
 assert(pw_d3d9_private_set(&store,&key,source,1,0)==E_OUTOFMEMORY);
 assert(pw_d3d9_private_set(&store,&key,source,PW_D3D9_PRIVATE_MAX_BYTES,0)==E_OUTOFMEMORY);
 pw_d3d9_private_dispose(&store);assert(!store.count);
 assert(pw_d3d9_private_set(&store,&key,source,1,0)==D3DERR_INVALIDCALL);
 struct pw_d3d9_private_data final={0};assert(SUCCEEDED(pw_d3d9_private_set(&final,&key,&object,sizeof(void *),D3DSPD_IUNKNOWN))&&references==2);
 pw_d3d9_private_dispose(&final);assert(references==1);
 puts("PW_PRIVATE_DATA PASS copied=1 local_unknown=1 reentrant=1 bounded=1 cleanup=1");return 0;
}
