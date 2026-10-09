/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "wine/ps5/pw_d3d9_factory_wire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <d3d9.h>
#endif
static void test_requests(void)
{
 struct pw_d3d9_factory_request in={0},out={0};unsigned char wire[64],again[64];
 size_t bytes,other,n;uint32_t method;
 for(method=4;method<=14;method++){
  in=(struct pw_d3d9_factory_request){method,3,2,21,22,23,0x12345678,4,9,0x87654321,1,8};
  memset(wire,0xa5,sizeof(wire));
  assert(pw_d3d9_factory_request_encode(wire,0,&bytes,&in)==PW_D3D9_FACTORY_SMALL);
  assert(bytes>=16&&bytes<=40&&wire[0]==0xa5);
  assert(!pw_d3d9_factory_request_encode(wire,sizeof(wire),&bytes,&in));
  assert(wire[0]==1&&wire[4]==method&&wire[12]==0);
  for(n=0;n<bytes;n++)assert(pw_d3d9_factory_request_decode(&out,wire,n)==PW_D3D9_FACTORY_INVALID);
  assert(!pw_d3d9_factory_request_decode(&out,wire,bytes));
  assert(!pw_d3d9_factory_request_encode(again,sizeof(again),&other,&out));
  assert(bytes==other&&!memcmp(wire,again,bytes));
  assert(pw_d3d9_factory_request_decode(&out,wire,bytes+1)==PW_D3D9_FACTORY_INVALID);
  wire[12]=1;assert(pw_d3d9_factory_request_decode(&out,wire,bytes)==PW_D3D9_FACTORY_INVALID);
 }
 for(method=3;method<=16;method++)if(method==3||method==15||method==16){
  in.method=method;assert(pw_d3d9_factory_request_encode(wire,sizeof(wire),&bytes,&in)==PW_D3D9_FACTORY_UNSUPPORTED);assert(!bytes);
 }
 in.method=17;assert(pw_d3d9_factory_request_encode(wire,sizeof(wire),&bytes,&in)==PW_D3D9_FACTORY_INVALID);
 in.method=9;in.windowed=2;assert(pw_d3d9_factory_request_encode(wire,sizeof(wire),&bytes,&in)==PW_D3D9_FACTORY_INVALID);
 in.windowed=1;assert(!pw_d3d9_factory_request_encode(wire,sizeof(wire),&bytes,&in));
 wire[32]=2;assert(pw_d3d9_factory_request_decode(&out,wire,bytes)==PW_D3D9_FACTORY_INVALID);
}
static void test_replies(void)
{
 struct pw_d3d9_factory_reply in={0},out={0};unsigned char wire[1200],again[1200];
 size_t bytes,other,n;uint32_t method,i=0;
 in.identifier.driver_version=UINT64_C(0xfedcba9876543210);
 strcpy(in.identifier.driver,"native backend");strcpy(in.identifier.description,"device description");strcpy(in.identifier.device_name,"display0");
 in.identifier.vendor_id=0x12345678;in.identifier.device_id=9;in.identifier.subsystem_id=10;in.identifier.revision=11;
 in.identifier.guid_data1=0x87654321;in.identifier.guid_data2=0x1234;in.identifier.guid_data3=0xabcd;
 for(i=0;i<8;i++)in.identifier.guid_data4[i]=(unsigned char)(i+1);
 in.identifier.whql_level=0x11223344;in.count=UINT32_MAX;
 in.mode=(struct pw_d3d9_display_mode){1920,1080,120,21};i=0;
#define PW_CAP_FILL(type,name,native) in.caps.name=UINT32_C(0xdead0000)+i++;
 PW_D3D9_CAP_FIELDS(PW_CAP_FILL)
#undef PW_CAP_FILL
 assert(i==76);in.caps.MaxVertexW=0x7fc12345;in.caps.VS20Caps_NumTemps=UINT32_MAX;
 for(method=4;method<=14;method++){
  in.method=method;in.hresult=0;memset(wire,0xa5,sizeof(wire));
  assert(pw_d3d9_factory_reply_encode(wire,0,&bytes,&in)==PW_D3D9_FACTORY_SMALL);
  assert(bytes<=PW_D3D9_FACTORY_MAX_REPLY&&wire[0]==0xa5);
  assert(!pw_d3d9_factory_reply_encode(wire,sizeof(wire),&bytes,&in));
  for(n=0;n<bytes;n++)assert(pw_d3d9_factory_reply_decode(&out,wire,n)==PW_D3D9_FACTORY_INVALID);
  assert(!pw_d3d9_factory_reply_decode(&out,wire,bytes));
  assert(!pw_d3d9_factory_reply_encode(again,sizeof(again),&other,&out));
  assert(bytes==other&&!memcmp(wire,again,bytes));
  assert(pw_d3d9_factory_reply_decode(&out,wire,bytes+1)==PW_D3D9_FACTORY_INVALID);
  if(method==5){assert(bytes==1116&&out.identifier.driver_version==in.identifier.driver_version);assert(wire[1096]==0x21&&wire[1100]==0x34&&wire[1102]==0xcd);}
  if(method==14){assert(bytes==320&&out.caps.MaxVertexW==0x7fc12345&&out.caps.VS20Caps_NumTemps==UINT32_MAX);}
  in.hresult=0x87654321;
  if(method==4||method==6){assert(pw_d3d9_factory_reply_encode(wire,sizeof(wire),&bytes,&in)==PW_D3D9_FACTORY_INVALID);continue;}
  assert(!pw_d3d9_factory_reply_encode(wire,sizeof(wire),&bytes,&in)&&bytes==16);
  memset(&out,0xa5,sizeof(out));assert(!pw_d3d9_factory_reply_decode(&out,wire,bytes));
  assert(out.hresult==0x87654321&&out.count==0&&!out.identifier.driver[0]&&!out.caps.Caps);
  wire[12]=4;assert(pw_d3d9_factory_reply_decode(&out,wire,bytes)==PW_D3D9_FACTORY_INVALID);
 }
 in.method=5;in.hresult=0;memset(in.identifier.driver,'x',512);
 assert(pw_d3d9_factory_reply_encode(wire,sizeof(wire),&bytes,&in)==PW_D3D9_FACTORY_INVALID);
 in.identifier.driver[0]=0; /* Nonzero local string tails must not leak. */
 assert(!pw_d3d9_factory_reply_encode(wire,sizeof(wire),&bytes,&in));
 for(n=16;n<528;n++)assert(!wire[n]);
 wire[17]=1;assert(pw_d3d9_factory_reply_decode(&out,wire,bytes)==PW_D3D9_FACTORY_INVALID);
 in.method=16;assert(pw_d3d9_factory_reply_encode(wire,sizeof(wire),&bytes,&in)==PW_D3D9_FACTORY_UNSUPPORTED);
#ifdef _WIN32
 {D3DCAPS9 native;size_t index=0;
#define PW_CAP_NATIVE(type,name,member) assert(sizeof(native.member)==4);assert(offsetof(D3DCAPS9,member)==4*index++);memcpy(&native.member,&in.caps.name,4);
  PW_D3D9_CAP_FIELDS(PW_CAP_NATIVE)
#undef PW_CAP_NATIVE
  assert(index==76&&sizeof(native)==304);
 }
#endif
}
int main(void){test_requests();test_replies();puts("D3D9 factory wire: PASS");return 0;}
