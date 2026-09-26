/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_wine_prx.h"
#include <string.h>

static int inside(const PwPrxSegment *segments,uint32_t count,const void *pointer,size_t bytes)
{
    uintptr_t address=(uintptr_t)pointer;
    if(!pointer || !bytes || address>UINTPTR_MAX-bytes)return 0;
    for(uint32_t i=0;i<count;i++) {
        uintptr_t base=(uintptr_t)segments[i].address;
        if(base && address>=base && address-base<=segments[i].size &&
           bytes<=segments[i].size-(address-base))return 1;
    }
    return 0;
}
static int valid_name(const PwPrxSegment *segments,uint32_t count,const char *name)
{
    for(size_t length=0;length<PW_PRX_MAX_NAME;length++) {
        if(!inside(segments,count,name+length,1))return 0;
        if(!name[length])return length>0;
    }
    return 0;
}
static int valid(const PwPrxSegment *segments,uint32_t count,const PwPrxDescriptor *d)
{
    if(d->version!=PW_PRX_VERSION || !d->count || d->count>PW_PRX_MAX_EXPORTS ||
       !inside(segments,count,d,sizeof(*d)+(size_t)d->count*sizeof(PwPrxExport)))return 0;
    for(uint32_t i=0;i<d->count;i++)
        if(!valid_name(segments,count,d->exports[i].name) ||
           !inside(segments,count,d->exports[i].address,1))return 0;
    return 1;
}

int pw_prx_parse_module_info(const void *info,char name[PW_PRX_MAX_NAME],
                             PwPrxSegment segments[PW_PRX_MAX_SEGMENTS],uint32_t *count)
{
    if(!info || !segments || !count)return PW_PRX_ERR_ARGUMENT;
    const uint8_t *raw=info;uint64_t size;uint32_t n;
    memcpy(&size,raw,sizeof(size));memcpy(&n,raw+0x148,sizeof(n));
    /* The caller sets the size word; FW 12.02 clears it on success (measured). */
    if((size && size!=PW_PRX_MODULE_INFO_BYTES) || !n || n>PW_PRX_MAX_SEGMENTS)
        return PW_PRX_ERR_MODULE_INFO;
    for(uint32_t i=0;i<n;i++) {
        const uint8_t *entry=raw+0x108+i*16u;uint64_t address;
        memcpy(&address,entry,sizeof(address));
        memcpy(&segments[i].size,entry+8,sizeof(uint32_t));
        memcpy(&segments[i].protection,entry+12,sizeof(uint32_t));
        segments[i].address=(const void *)(uintptr_t)address;
        if(!address || !segments[i].size)return PW_PRX_ERR_MODULE_INFO;
    }
    if(name){memcpy(name,raw+8,PW_PRX_MAX_NAME-1);name[PW_PRX_MAX_NAME-1]=0;}
    *count=n;return PW_PRX_OK;
}

int pw_prx_find_descriptor(const PwPrxSegment *segments,uint32_t count,
                           const PwPrxDescriptor **descriptor)
{
    if(!segments || !descriptor || !count || count>PW_PRX_MAX_SEGMENTS)return PW_PRX_ERR_ARGUMENT;
    *descriptor=NULL;
    for(uint32_t i=0;i<count;i++) {
        const uint8_t *base=segments[i].address;
        if(!base || !(segments[i].protection&1u) || ((uintptr_t)base&(PW_PRX_ALIGN-1)))continue;
        for(size_t offset=0;offset+sizeof(PwPrxDescriptor)<=segments[i].size;offset+=PW_PRX_ALIGN) {
            uint64_t magic;memcpy(&magic,base+offset,sizeof(magic));
            if(magic!=PW_PRX_MAGIC)continue;
            const PwPrxDescriptor *candidate=(const PwPrxDescriptor *)(base+offset);
            if(valid(segments,count,candidate)){*descriptor=candidate;return PW_PRX_OK;}
        }
    }
    return PW_PRX_ERR_NO_DESCRIPTOR;
}

const void *pw_prx_lookup(const PwPrxDescriptor *descriptor,const char *name)
{
    if(!descriptor || !name)return NULL;
    for(uint32_t i=0;i<descriptor->count;i++)
        if(!strcmp(descriptor->exports[i].name,name))return descriptor->exports[i].address;
    return NULL;
}
