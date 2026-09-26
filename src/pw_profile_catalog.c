/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_profile_catalog.h"
#include <string.h>

static const char suffix[]=".profile";

static int valid_name(const char *name,size_t length)
{
    const size_t tail=sizeof(suffix)-1u;
    if(length<=tail || length>=PW_PROFILE_CATALOG_NAME ||
       memcmp(name+length-tail,suffix,tail))return 0;
    for(size_t i=0;i<length-tail;i++) {
        char c=name[i];
        if(!((c>='a' && c<='z') || (c>='0' && c<='9') || c=='_' || c=='-'))return 0;
    }
    return 1;
}

int pw_profile_catalog_parse(const uint8_t *text,size_t bytes,PwProfileCatalog *catalog)
{
    if(!catalog || (!text && bytes))return PW_ERR_PRECONDITION;
    PwProfileCatalog parsed;memset(&parsed,0,sizeof(parsed));
    size_t start=0;
    while(start<bytes) {
        size_t end=start;
        while(end<bytes && text[end]!='\n')end++;
        size_t first=start,last=end;
        while(first<last && (text[first]==' ' || text[first]=='\t'))first++;
        while(last>first && (text[last-1]==' ' || text[last-1]=='\t' || text[last-1]=='\r'))last--;
        if(first<last && text[first]!='#' && text[first]!=';') {
            const char *name=(const char *)text+first;size_t length=last-first;
            if(!valid_name(name,length))return PW_ERR_MALFORMED;
            for(uint32_t i=0;i<parsed.count;i++)
                if(!strncmp(parsed.names[i],name,length) && !parsed.names[i][length])
                    return PW_ERR_MALFORMED;
            if(parsed.count==PW_PROFILE_CATALOG_MAX)return PW_ERR_LIMIT;
            memcpy(parsed.names[parsed.count++],name,length);
        }
        start=end+1;
    }
    *catalog=parsed;return PW_OK;
}

int pw_profile_catalog_launchable(const PwAppProfile *profile)
{
    return profile && !strcmp(profile->runtime,"prospero-win-direct") &&
        profile->architecture==PW_APP_ARCH_PE32 && profile->graphics==PW_APP_GRAPHICS_GDI;
}
