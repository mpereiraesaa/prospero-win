/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _DEFAULT_SOURCE
#include "../native/pw_prefix_ps5.h"
#include "../src/pw_app_profile.h"
#include "../src/pw_registry_store.h"
#include "../src/pw_runtime_supervisor.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void load_profile(const char *path,PwAppProfile *profile)
{
    uint8_t data[PW_APP_PROFILE_MAX_BYTES];
    FILE *file=fopen(path,"rb");assert(file);
    size_t bytes=fread(data,1,sizeof(data),file);
    assert(!ferror(file));assert(fgetc(file)==EOF);assert(!ferror(file));
    assert(fclose(file)==0);
    assert(pw_app_profile_parse(data,bytes,profile)==PW_OK);
}

static void save_snapshot(const PwPrefixLayout *prefix,const char *profile_id,
                          char *path,size_t capacity)
{
    PwRegistryKey keys[2],loaded_keys[2];
    PwRegistryValue values[4],loaded_values[4];
    PwRegistry registry,loaded;uint8_t bytes[1024];uint32_t encoded=0;
    uint32_t key=0,disposition=0;
    assert(snprintf(path,capacity,"%s/registry.pwrg",prefix->root)>0);
    assert(pw_registry_init(&registry,keys,2,values,4)==PW_OK);
    assert(pw_registry_create(&registry,PW_HKEY_CURRENT_USER,
        "Software\\Prospero",&key,&disposition)==PW_REG_ERROR_SUCCESS);
    assert(pw_registry_set(&registry,key,"Profile",PW_REG_SZ,profile_id,
        (uint32_t)strlen(profile_id)+1u)==PW_REG_ERROR_SUCCESS);
    assert(pw_registry_encode(&registry,bytes,sizeof(bytes),&encoded)==PW_OK);
    FILE *file=fopen(path,"wb");assert(file);
    assert(fwrite(bytes,1,encoded,file)==encoded);assert(fclose(file)==0);

    assert(pw_registry_init(&loaded,loaded_keys,2,loaded_values,4)==PW_OK);
    file=fopen(path,"rb");assert(file);
    uint8_t reread[1024];size_t read_bytes=fread(reread,1,sizeof(reread),file);
    assert(!ferror(file));assert(fgetc(file)==EOF);assert(fclose(file)==0);
    assert(pw_registry_decode(&loaded,reread,(uint32_t)read_bytes)==PW_OK);
    assert(pw_registry_open(&loaded,PW_HKEY_CURRENT_USER,
        "software/prospero",&key)==PW_REG_ERROR_SUCCESS);
    char value[PW_APP_ID_CAPACITY];uint32_t type=0,size=sizeof(value);
    assert(pw_registry_query(&loaded,key,"profile",&type,value,&size)==
        PW_REG_ERROR_SUCCESS);
    assert(type==PW_REG_SZ && strcmp(value,profile_id)==0);
}

static void remove_prefix(const PwPrefixLayout *prefix)
{
    char path[PW_PREFIX_PATH_CAPACITY];
    assert(rmdir(prefix->temp)==0);
    assert(snprintf(path,sizeof(path),"%s/AppData/Local",prefix->user_home)>0);
    assert(rmdir(path)==0);
    assert(snprintf(path,sizeof(path),"%s/AppData",prefix->user_home)>0);
    assert(rmdir(path)==0);assert(rmdir(prefix->user_home)==0);
    assert(rmdir(prefix->users)==0);assert(rmdir(prefix->program_files_x86)==0);
    assert(rmdir(prefix->program_files)==0);assert(rmdir(prefix->system32)==0);
    assert(rmdir(prefix->system)==0);assert(rmdir(prefix->windows)==0);
    assert(rmdir(prefix->drive_c)==0);assert(rmdir(prefix->root)==0);
}

int main(void)
{
    static const char *const manifests[]={
        "examples/profiles/pinball.profile","examples/profiles/paint.profile"};
    char temporary[]="/tmp/prospero-session-flow-XXXXXX";
    char storage[PW_PREFIX_PATH_CAPACITY],snapshot_paths[2][PW_PREFIX_PATH_CAPACITY];
    PwAppProfile profiles[2];PwPrefixLayout prefixes[2];
    assert(mkdtemp(temporary));
    assert(snprintf(storage,sizeof(storage),"%s/storage",temporary)>0);
    PwPrefixIo io;PwPrefixService service;
    assert(pw_prefix_ps5_io(&io)==PW_OK);
    assert(pw_prefix_service_init(&service,storage,&io)==PW_OK);
    for(unsigned i=0;i<2;i++) {
        load_profile(manifests[i],&profiles[i]);
        assert(profiles[i].architecture==PW_APP_ARCH_PE32);
        assert(profiles[i].graphics==PW_APP_GRAPHICS_GDI);
        assert(pw_prefix_open(&service,profiles[i].prefix,&prefixes[i])==PW_OK);
    }
    assert(strcmp(prefixes[0].root,prefixes[1].root)!=0);

    PwRuntimeSupervisor supervisor;pw_runtime_supervisor_init(&supervisor);
    assert(pw_runtime_supervisor_begin(&supervisor,&profiles[0],&prefixes[0])==PW_OK);
    assert(pw_runtime_supervisor_begin(&supervisor,&profiles[1],&prefixes[1])==PW_ERR_STATE);
    assert(pw_runtime_supervisor_guest_started(&supervisor)==PW_OK);
    assert(pw_runtime_supervisor_request_stop(&supervisor)==PW_OK);
    assert(pw_runtime_supervisor_guest_exited(&supervisor,0)==PW_OK);
    assert(pw_runtime_supervisor_cleanup_complete(&supervisor,PW_OK)==PW_OK);
    assert(supervisor.state==PW_RUNTIME_IDLE);

    assert(pw_runtime_supervisor_begin(&supervisor,&profiles[1],&prefixes[1])==PW_OK);
    assert(supervisor.last_result.session_id==2u);
    assert(pw_runtime_supervisor_guest_started(&supervisor)==PW_OK);
    assert(pw_runtime_supervisor_request_stop(&supervisor)==PW_OK);
    assert(pw_runtime_supervisor_guest_exited(&supervisor,0)==PW_OK);
    assert(pw_runtime_supervisor_cleanup_complete(&supervisor,PW_OK)==PW_OK);

    save_snapshot(&prefixes[0],profiles[0].id,snapshot_paths[0],
                  sizeof(snapshot_paths[0]));
    save_snapshot(&prefixes[1],profiles[1].id,snapshot_paths[1],
                  sizeof(snapshot_paths[1]));
    assert(strcmp(snapshot_paths[0],snapshot_paths[1])!=0);
    for(unsigned i=0;i<2;i++) {
        struct stat status;assert(stat(snapshot_paths[i],&status)==0);
        assert(status.st_size>0);assert(unlink(snapshot_paths[i])==0);
        remove_prefix(&prefixes[i]);
    }
    assert(snprintf(storage,sizeof(storage),"%s/storage/prefixes",temporary)>0);
    assert(rmdir(storage)==0);
    assert(snprintf(storage,sizeof(storage),"%s/storage",temporary)>0);
    assert(rmdir(storage)==0);assert(rmdir(temporary)==0);
    return 0;
}
