/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_prefix_ps5.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef PW_PREFIX_PS5_HOST_TEST
static int platform_stat(const char *path,struct stat *status)
{return stat(path,status);}
static int platform_mkdir(const char *path,int mode)
{return mkdir(path,(mode_t)mode);}
#else
extern int sceKernelStat(const char *path,struct stat *status);
extern int sceKernelMkdir(const char *path,int mode);
#define platform_stat sceKernelStat
#define platform_mkdir sceKernelMkdir
#endif

static int ensure_directory(const char *path)
{
    struct stat status;
    if(platform_stat(path,&status)==0)
        return S_ISDIR(status.st_mode)?PW_OK:PW_ERR_STATE;
    if(platform_mkdir(path,0700)==0)return PW_OK;
    /* Another creator may have won the race; only an actual directory is
     * success. This also distinguishes EEXIST-on-file from idempotence. */
    if(platform_stat(path,&status)==0 && S_ISDIR(status.st_mode))return PW_OK;
    return PW_ERR_STATE;
}

int pw_prefix_ps5_make_directories(void *context,const char *path)
{
    char copy[PW_PREFIX_PATH_CAPACITY];
    size_t length;
    (void)context;
    if(!path || path[0]!='/')return PW_ERR_PRECONDITION;
    length=strlen(path);
    if(length==0u || length>=sizeof(copy))return PW_ERR_LIMIT;
    if(length>1u && path[length-1u]=='/')return PW_ERR_MALFORMED;
    for(size_t i=0;i<length;i++) {
        unsigned char value=(unsigned char)path[i];
        if(value<0x20u || value=='\\')return PW_ERR_MALFORMED;
        if(value=='/' && i && path[i-1u]=='/')return PW_ERR_MALFORMED;
    }
    memcpy(copy,path,length+1u);
    if(length==1u)return ensure_directory(copy);
    for(size_t i=1u;i<=length;i++) {
        if(copy[i]!='/' && copy[i]!='\0')continue;
        char saved=copy[i];copy[i]='\0';
        size_t component_start=i;
        while(component_start>0u && copy[component_start-1u]!='/')
            component_start--;
        size_t component_length=i-component_start;
        if((component_length==1u && copy[component_start]=='.') ||
           (component_length==2u && copy[component_start]=='.' &&
            copy[component_start+1u]=='.'))return PW_ERR_MALFORMED;
        int status=ensure_directory(copy);
        copy[i]=saved;
        if(status!=PW_OK)return status;
    }
    return PW_OK;
}

int pw_prefix_ps5_io(PwPrefixIo *io)
{
    if(!io)return PW_ERR_PRECONDITION;
    *io=(PwPrefixIo){.context=NULL,.make_directories=pw_prefix_ps5_make_directories};
    return PW_OK;
}
