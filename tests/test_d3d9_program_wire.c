/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_program_wire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void put(unsigned char *p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
static int reader(void *ctx,size_t i,uint32_t *v){*v=((uint32_t *)ctx)[i];return 0;}
static void token_bytes(unsigned char *p,const uint32_t *v,size_t n){size_t i;for(i=0;i<n;i++)put(p+4*i,v[i]);}
static void measurement(void)
{
 uint32_t vs[]={0xfffe0300,0x0002fffe,0xffff,0xffff,0x05000051,0xa00f0000,0xffff,0,0,0,0xffff};
 uint32_t old[]={0xffff0104,66,0x800f0000,0xb0e40000,0xffff};
 uint32_t oldvs[]={0xfffe0101,1,0xc00f0000,0x90e40000,0xffff};
 unsigned char bytes[128],decl[16]={0,0,0,0,2,0,0,0,255,0,0,0,17,0,0,0};size_t n=99,i;
 assert(!pw_d3d9_program_measure(PW_D3D9_PROGRAM_VS,reader,vs,11,&n)&&n==11);
 for(i=0;i<11;i++){n=99;assert(pw_d3d9_program_measure(PW_D3D9_PROGRAM_VS,reader,vs,i,&n)&&n==99);}
 token_bytes(bytes,vs,11);assert(!pw_d3d9_program_validate(PW_D3D9_PROGRAM_VS,bytes,44));
 assert(pw_d3d9_program_validate(PW_D3D9_PROGRAM_PS,bytes,44));
 put(bytes+44,0);assert(pw_d3d9_program_validate(PW_D3D9_PROGRAM_VS,bytes,48));
 assert(!pw_d3d9_program_measure(PW_D3D9_PROGRAM_PS,reader,old,5,&n)&&n==5);
 assert(!pw_d3d9_program_measure(PW_D3D9_PROGRAM_VS,reader,oldvs,5,&n)&&n==5);
 old[1]=0x1234;assert(pw_d3d9_program_measure(PW_D3D9_PROGRAM_PS,reader,old,5,&n));
 assert(!pw_d3d9_program_validate(PW_D3D9_PROGRAM_DECL,decl,16));
 for(i=0;i<16;i++)assert(pw_d3d9_program_validate(PW_D3D9_PROGRAM_DECL,decl,i));
 decl[12]=16;assert(pw_d3d9_program_validate(PW_D3D9_PROGRAM_DECL,decl,16));
 decl[12]=17;decl[0]=16;assert(pw_d3d9_program_validate(PW_D3D9_PROGRAM_DECL,decl,16));
}
static void wire(void)
{
 struct pw_d3d9_program_request q={0},r,before;
 struct pw_d3d9_program_reply reply={PW_D3D9_PROGRAM_COMMIT,0,5,7,UINT64_C(0x1234567800000001)},decoded;
 unsigned char bytes[PW_D3D9_PROGRAM_WIRE_MAX];size_t n=77,i;
 q.operation=PW_D3D9_PROGRAM_WRITE;q.kind=PW_D3D9_PROGRAM_PS;q.transfer=UINT64_C(0x100000003);q.count=4096;
 memset(q.data,0xa5,sizeof(q.data));assert(!pw_d3d9_program_encode(bytes,sizeof(bytes),&n,&q)&&n==sizeof(bytes));
 assert(!pw_d3d9_program_decode(&r,bytes,n)&&!memcmp(&q,&r,sizeof(q)));
 memset(&before,0xa6,sizeof(before));
 for(i=0;i<n;i++){r=before;assert(pw_d3d9_program_decode(&r,bytes,i));assert(!memcmp(&r,&before,sizeof(r)));}
 bytes[0]=2;assert(pw_d3d9_program_decode(&r,bytes,n));bytes[0]=1;
 n=99;assert(pw_d3d9_program_encode(bytes,31,&n,&q)==PW_D3D9_PROGRAM_SMALL&&n==99);
 q.offset=UINT32_MAX;assert(pw_d3d9_program_encode(bytes,sizeof(bytes),&n,&q));
 assert(!pw_d3d9_program_reply_encode(bytes,sizeof(bytes),&n,&reply)&&n==32);
 assert(!pw_d3d9_program_reply_decode(&decoded,bytes,n)&&decoded.transfer==reply.transfer&&decoded.id==5);
 for(i=0;i<32;i++)assert(pw_d3d9_program_reply_decode(&decoded,bytes,i));
 reply.hresult=0x8876086c;assert(pw_d3d9_program_reply_encode(bytes,sizeof(bytes),&n,&reply));
 reply.id=reply.generation=0;reply.transfer=0;assert(!pw_d3d9_program_reply_encode(bytes,sizeof(bytes),&n,&reply));
 assert(!pw_d3d9_program_reply_decode(&decoded,bytes,n)&&decoded.hresult==reply.hresult);
}
static void upload(void)
{
 unsigned char storage[PW_D3D9_PROGRAM_LIMIT];struct pw_d3d9_program_upload u;
 struct pw_d3d9_program_request q={0};uint64_t id=99,old;uint32_t offset;
 pw_d3d9_program_upload_init(&u,storage,sizeof(storage));q.operation=PW_D3D9_PROGRAM_BEGIN;q.kind=PW_D3D9_PROGRAM_VS;q.total=sizeof(storage);
 assert(!pw_d3d9_program_upload_apply(&u,&q,&id)&&id==1);
 assert(pw_d3d9_program_upload_apply(&u,&q,&id)==PW_D3D9_PROGRAM_BUSY);
 old=id;q.operation=PW_D3D9_PROGRAM_COMMIT;q.total=0;q.transfer=id;
 assert(pw_d3d9_program_upload_apply(&u,&q,&id)==PW_D3D9_PROGRAM_INVALID&&!u.ready);
 q.operation=PW_D3D9_PROGRAM_WRITE;q.count=4096;
 for(offset=0;offset<sizeof(storage);offset+=4096){
  memset(q.data,0,sizeof(q.data));if(!offset)put(q.data,0xfffe0300);if(offset+4096==sizeof(storage))put(q.data+4092,0xffff);
  q.offset=offset+1;assert(pw_d3d9_program_upload_apply(&u,&q,&id)==PW_D3D9_PROGRAM_INVALID&&u.received==offset);
  q.offset=offset;assert(!pw_d3d9_program_upload_apply(&u,&q,&id));
 }
 q.operation=PW_D3D9_PROGRAM_COMMIT;q.offset=q.count=0;
 assert(!pw_d3d9_program_upload_apply(&u,&q,&id)&&u.ready&&u.received==sizeof(storage));
 assert(pw_d3d9_program_upload_apply(&u,&q,&id)==PW_D3D9_PROGRAM_BUSY);
 pw_d3d9_program_upload_finish(&u);assert(pw_d3d9_program_upload_apply(&u,&q,&id)==PW_D3D9_PROGRAM_STALE);
 q.operation=PW_D3D9_PROGRAM_BEGIN;q.transfer=0;q.total=8;assert(!pw_d3d9_program_upload_apply(&u,&q,&id)&&id>old);
 q.operation=PW_D3D9_PROGRAM_ABORT;q.total=0;q.transfer=old;assert(pw_d3d9_program_upload_apply(&u,&q,&id)==PW_D3D9_PROGRAM_STALE);
 q.transfer=u.transfer;assert(!pw_d3d9_program_upload_apply(&u,&q,&id)&&!u.transfer);
 u.next=UINT64_MAX;q.operation=PW_D3D9_PROGRAM_BEGIN;q.transfer=0;q.total=8;
 assert(pw_d3d9_program_upload_apply(&u,&q,&id)==PW_D3D9_PROGRAM_EXHAUSTED);
}
int main(void){measurement();wire();upload();puts("PASS program payloads, token boundaries, chunk ordering, stale transfer and atomic outputs");return 0;}
