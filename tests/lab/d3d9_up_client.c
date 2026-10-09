/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_up_client.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static IDirect3DDevice9 device;static LONG refs=1;static unsigned calls,draws,aborts,failures,mode;static unsigned char source[8192],storage[8192],expected[8192];static struct pw_d3d9_up_upload upload;
static ULONG WINAPI addref(IDirect3DDevice9 *d){assert(d==&device);return ++refs;}
static ULONG WINAPI release(IDirect3DDevice9 *d){assert(d==&device&&refs);return --refs;}
static void fail(IDirect3DDevice9 *d,HRESULT hr){assert(d==&device&&refs&&FAILED(hr));failures++;}
static HRESULT up(IDirect3DDevice9 *d,const struct pw_d3d9_up_request *q,struct pw_d3d9_up_reply *r)
{
 unsigned char wire[PW_D3D9_UP_WIRE_MAX];struct pw_d3d9_up_request decoded;size_t n;uint64_t token;HRESULT hr=S_OK;
 assert(d==&device&&refs);calls++;assert(!pw_d3d9_up_encode(wire,sizeof(wire),&n,q));assert(!pw_d3d9_up_decode(&decoded,wire,n));
 if(q->operation==PW_D3D9_UP_BEGIN){memset(source,0xee,sizeof(source));if(mode==4){assert(refs==2);release(d);}}
 if(mode==1&&q->operation==PW_D3D9_UP_WRITE)return D3DERR_DEVICELOST;
 assert(!pw_d3d9_up_upload_apply(&upload,&decoded,&token));
 if(q->operation==PW_D3D9_UP_COMMIT){assert(upload.ready&&!memcmp(upload.storage,expected,upload.total));draws++;pw_d3d9_up_upload_finish(&upload);if(mode==2)return D3DERR_INVALIDCALL;hr=0x1234;}
 if(q->operation==PW_D3D9_UP_ABORT)aborts++;
 *r=(struct pw_d3d9_up_reply){q->operation,(uint32_t)hr,token};if(mode==3)r->operation=99;
 return hr;
}
int main(void)
{
 IDirect3DDevice9Vtbl table={.AddRef=addref,.Release=release};struct pw_d3d9_up_client_ops ops={up,fail};unsigned i,before;HRESULT hr;
 device.lpVtbl=&table;pw_d3d9_up_client_install(&table,&ops);pw_d3d9_up_upload_init(&upload,storage,sizeof(storage));
 for(i=0;i<sizeof(source);i++)source[i]=expected[i]=(unsigned char)i;
 hr=table.DrawPrimitiveUP(&device,D3DPT_TRIANGLELIST,100,source,20);assert(hr==0x1234&&draws==1&&calls==4&&refs==1);
 memcpy(source,expected,sizeof(source));hr=table.DrawIndexedPrimitiveUP(&device,D3DPT_TRIANGLELIST,2,3,1,source+100,D3DFMT_INDEX16,source,20);assert(hr==0x1234&&draws==2);
 before=calls;assert(table.DrawPrimitiveUP(&device,D3DPT_TRIANGLELIST,UINT32_MAX,source,20)==D3DERR_INVALIDCALL&&calls==before);
 assert(table.DrawPrimitiveUP(&device,D3DPT_TRIANGLELIST,1,NULL,20)==D3DERR_INVALIDCALL&&calls==before);
 assert(table.DrawPrimitiveUP(&device,D3DPT_TRIANGLELIST,0,NULL,20)==0x1234&&draws==3);
 mode=1;memcpy(source,expected,sizeof(source));assert(table.DrawPrimitiveUP(&device,D3DPT_TRIANGLELIST,1,source,20)==D3DERR_DEVICELOST&&aborts==1&&!upload.transfer);
 mode=2;memcpy(source,expected,sizeof(source));assert(table.DrawPrimitiveUP(&device,D3DPT_TRIANGLELIST,1,source,20)==D3DERR_INVALIDCALL&&!upload.transfer&&aborts==1);
 mode=3;assert(table.DrawPrimitiveUP(&device,D3DPT_TRIANGLELIST,0,NULL,20)==E_FAIL&&failures==1);pw_d3d9_up_upload_finish(&upload);
 mode=4;assert(table.DrawPrimitiveUP(&device,D3DPT_TRIANGLELIST,0,NULL,20)==0x1234&&refs==0);
 puts("PASS UP frontend: owned snapshot before callbacks, multi-chunk/prefix, exact HRESULT, abort, zero draw, overflow/null, protocol failure, device pin");return 0;
}
