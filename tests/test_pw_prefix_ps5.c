/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _DEFAULT_SOURCE
#include "../native/pw_prefix_ps5.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void is_directory(const char *path)
{
    struct stat status;
    assert(stat(path,&status)==0);
    assert(S_ISDIR(status.st_mode));
}

int main(void)
{
    char temporary[]="/tmp/prospero-prefix-XXXXXX";
    char storage[PW_PREFIX_PATH_CAPACITY];
    assert(mkdtemp(temporary));
    assert(snprintf(storage,sizeof(storage),"%s/storage",temporary)>0);

    PwPrefixIo io;PwPrefixService service;PwPrefixLayout first,again;
    assert(pw_prefix_ps5_io(&io)==PW_OK);
    assert(pw_prefix_service_init(&service,storage,&io)==PW_OK);
    assert(pw_prefix_open(&service,"pinball",&first)==PW_OK);
    assert(pw_prefix_open(&service,"pinball",&again)==PW_OK);
    assert(strcmp(first.root,again.root)==0);
    is_directory(first.root);is_directory(first.drive_c);
    is_directory(first.windows);is_directory(first.system32);
    is_directory(first.system);is_directory(first.program_files);
    is_directory(first.program_files_x86);is_directory(first.users);
    is_directory(first.user_home);is_directory(first.temp);

    assert(pw_prefix_ps5_make_directories(NULL,"relative/path")==
           PW_ERR_PRECONDITION);
    assert(pw_prefix_ps5_make_directories(NULL,"/tmp/a/../b")==
           PW_ERR_MALFORMED);
    assert(pw_prefix_ps5_make_directories(NULL,"/tmp//double")==
           PW_ERR_MALFORMED);

    char file_path[PW_PREFIX_PATH_CAPACITY];
    assert(snprintf(file_path,sizeof(file_path),"%s/not-a-directory",storage)>0);
    FILE *file=fopen(file_path,"wb");assert(file);assert(fclose(file)==0);
    assert(pw_prefix_ps5_make_directories(NULL,file_path)==PW_ERR_STATE);
    assert(unlink(file_path)==0);

    assert(rmdir(first.temp)==0);
    char path[PW_PREFIX_PATH_CAPACITY];
    assert(snprintf(path,sizeof(path),"%s/AppData/Local",first.user_home)>0);
    assert(rmdir(path)==0);
    assert(snprintf(path,sizeof(path),"%s/AppData",first.user_home)>0);
    assert(rmdir(path)==0);
    assert(rmdir(first.user_home)==0);assert(rmdir(first.users)==0);
    assert(rmdir(first.program_files_x86)==0);
    assert(rmdir(first.program_files)==0);assert(rmdir(first.system32)==0);
    assert(rmdir(first.system)==0);assert(rmdir(first.windows)==0);
    assert(rmdir(first.drive_c)==0);assert(rmdir(first.root)==0);
    assert(snprintf(path,sizeof(path),"%s/prefixes",storage)>0);
    assert(rmdir(path)==0);assert(rmdir(storage)==0);assert(rmdir(temporary)==0);
    return 0;
}
