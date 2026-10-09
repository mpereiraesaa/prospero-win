/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <assert.h>
#include <string.h>
#include "../wine/ps5/d3d9/pw_d3d9_failure_diag.h"
int main(void)
{
 FILE *f=tmpfile();assert(f);char text[2048]={0};
 struct pw_d3d9_factory_request factory={.method=10,.format=21,.format2=113,.usage=0x10000,.resource_type=3};
 struct pw_d3d9_texture_request texture={.operation=PW_D3D9_TEXTURE_SET_AUTOGEN_FILTER,.filter=2,.value=0xffffffffu};
 pw_d3d9_factory_failure(f,&factory,0);pw_d3d9_factory_failure(f,&factory,1);pw_d3d9_texture_failure(f,&texture,0);assert(ftell(f)==0);
 pw_d3d9_factory_failure(f,&factory,0x8876086au);pw_d3d9_texture_failure(f,&texture,0x8876086cu);
 rewind(f);size_t n=fread(text,1,sizeof(text)-1,f);assert(n>0&&!ferror(f));
 assert(strstr(text,"method=10 name=CheckDeviceFormat hr=8876086a"));assert(strstr(text,"format=21 format2=113"));assert(strstr(text,"usage=00010000 resource_type=3"));
 assert(strstr(text,"operation=24 name=SetAutoGenFilterType hr=8876086c"));assert(strstr(text,"filter=2 value=4294967295"));
 factory.method=UINT32_MAX;texture.operation=0;pw_d3d9_factory_failure(f,&factory,0x80004005u);pw_d3d9_texture_failure(f,&texture,0x80004005u);
 rewind(f);memset(text,0,sizeof(text));n=fread(text,1,sizeof(text)-1,f);assert(n>0&&!ferror(f));
 assert(strstr(text,"method=4294967295 name=unknown hr=80004005"));assert(strstr(text,"operation=0 name=unknown hr=80004005"));
 fclose(f);return 0;
}
