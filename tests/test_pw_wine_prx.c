/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../wine/ps5/pw_wine_prx.h"
#include <assert.h>
#include <string.h>

/* A synthetic module: one readable segment holding strings, a target and
 * several descriptor candidates, of which only the last is valid. */
static _Alignas(16) uint8_t module[4096];
static char outside_name[]="outside";

static PwPrxDescriptor *candidate(size_t offset,uint32_t version,uint32_t count)
{
    PwPrxDescriptor *d=(PwPrxDescriptor *)(module+offset);
    d->magic=PW_PRX_MAGIC;d->version=version;d->count=count;return d;
}

int main(void)
{
    char *funcs=(char *)module+64,*init=(char *)module+96;
    strcpy(funcs,"__wine_unix_call_funcs");strcpy(init,"__wine_unix_lib_init");
    const void *target=module+128,*other=module+144;
    /* Decoys, in scan order: wrong version, name outside the module,
     * address outside the module, count beyond the segment. */
    PwPrxDescriptor *d=candidate(256,2,1);d->exports[0]=(PwPrxExport){funcs,target};
    d=candidate(320,1,1);d->exports[0]=(PwPrxExport){outside_name,target};
    d=candidate(384,1,1);d->exports[0]=(PwPrxExport){funcs,&outside_name};
    candidate(448,1,1000);
    d=candidate(512,1,2);d->exports[0]=(PwPrxExport){funcs,target};d->exports[1]=(PwPrxExport){init,other};
    PwPrxSegment segment={module,sizeof(module),1};
    const PwPrxDescriptor *found=NULL;
    assert(pw_prx_find_descriptor(&segment,1,&found)==PW_PRX_OK && found==d);
    assert(pw_prx_lookup(found,"__wine_unix_call_funcs")==target);
    assert(pw_prx_lookup(found,"__wine_unix_lib_init")==other);
    assert(!pw_prx_lookup(found,"__wine_unix_call_wow64_funcs") && !pw_prx_lookup(found,NULL));
    /* An unreadable segment is not scanned; neither is an unterminated name. */
    segment.protection=4;assert(pw_prx_find_descriptor(&segment,1,&found)==PW_PRX_ERR_NO_DESCRIPTOR && !found);
    segment.protection=1;memset(init,'x',PW_PRX_MAX_NAME+8);
    assert(pw_prx_find_descriptor(&segment,1,&found)==PW_PRX_ERR_NO_DESCRIPTOR);
    strcpy(init,"__wine_unix_lib_init");
    /* A table that runs past the end of the segment is rejected. */
    PwPrxSegment short_segment={module,520,1};
    assert(pw_prx_find_descriptor(&short_segment,1,&found)==PW_PRX_ERR_NO_DESCRIPTOR);
    assert(pw_prx_find_descriptor(NULL,1,&found)==PW_PRX_ERR_ARGUMENT);
    assert(pw_prx_find_descriptor(&segment,0,&found)==PW_PRX_ERR_ARGUMENT);
    assert(pw_prx_find_descriptor(&segment,PW_PRX_MAX_SEGMENTS+1,&found)==PW_PRX_ERR_ARGUMENT);

    /* SceKernelModuleInfo parsing. */
    uint8_t info[PW_PRX_MODULE_INFO_BYTES]={0};uint64_t size=PW_PRX_MODULE_INFO_BYTES;
    memcpy(info,&size,8);strcpy((char *)info+8,"win32u.prx");
    uint64_t address=(uintptr_t)module;uint32_t bytes=4096,protection=5,count=2;
    memcpy(info+0x108,&address,8);memcpy(info+0x110,&bytes,4);memcpy(info+0x114,&protection,4);
    address+=16;protection=3;memcpy(info+0x118,&address,8);memcpy(info+0x120,&bytes,4);
    memcpy(info+0x124,&protection,4);memcpy(info+0x148,&count,4);
    char name[PW_PRX_MAX_NAME];PwPrxSegment segments[PW_PRX_MAX_SEGMENTS];uint32_t n=0;
    assert(pw_prx_parse_module_info(info,name,segments,&n)==PW_PRX_OK && n==2);
    assert(!strcmp(name,"win32u.prx") && segments[0].address==module && segments[0].size==4096 &&
           segments[0].protection==5 && segments[1].address==module+16 && segments[1].protection==3);
    count=5;memcpy(info+0x148,&count,4);
    assert(pw_prx_parse_module_info(info,name,segments,&n)==PW_PRX_ERR_MODULE_INFO);
    count=1;memcpy(info+0x148,&count,4);size=0x150;memcpy(info,&size,8);
    assert(pw_prx_parse_module_info(info,name,segments,&n)==PW_PRX_ERR_MODULE_INFO);
    size=PW_PRX_MODULE_INFO_BYTES;memcpy(info,&size,8);bytes=0;memcpy(info+0x110,&bytes,4);
    assert(pw_prx_parse_module_info(info,name,segments,&n)==PW_PRX_ERR_MODULE_INFO);
    assert(pw_prx_parse_module_info(NULL,name,segments,&n)==PW_PRX_ERR_ARGUMENT);
    return 0;
}
