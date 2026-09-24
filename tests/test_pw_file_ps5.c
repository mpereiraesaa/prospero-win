/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../native/pw_file_ps5.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int sceKernelOpen(const char *path, int flags, int mode)
{
    return open(path, flags, mode);
}

int sceKernelClose(int descriptor)
{
    return close(descriptor);
}

int sceKernelStat(const char *path, struct stat *status)
{
    return stat(path, status);
}

static void write_one(const char *path, unsigned char value)
{
    FILE *file = fopen(path, "wb");

    assert(file != NULL);
    assert(fwrite(&value, 1u, 1u, file) == 1u);
    assert(fclose(file) == 0);
}

int main(void)
{
    char root[128];
    char application[160];
    char staged_application[192];
    char staged_nested[224];
    char runtime[160];
    char path[256];
    PwFilePs5 state;
    PwFileProvider provider;
    PwFileSpan span;

    assert(snprintf(root, sizeof(root), "/tmp/pw_file_ps5_ns_%ld",
                    (long)getpid()) > 0);
    assert(snprintf(application, sizeof(application), "%s/application", root) > 0);
    assert(snprintf(staged_application,sizeof(staged_application),
                    "%s/app",application)>0);
    assert(snprintf(staged_nested,sizeof(staged_nested),
                    "%s/sub",staged_application)>0);
    assert(snprintf(runtime, sizeof(runtime), "%s/runtime", root) > 0);
    assert(mkdir(root, 0700) == 0);
    assert(mkdir(application, 0700) == 0);
    assert(mkdir(staged_application,0700)==0);
    assert(mkdir(staged_nested,0700)==0);
    assert(mkdir(runtime, 0700) == 0);

    assert(snprintf(path, sizeof(path), "%s/kernel32.dll", application) > 0);
    write_one(path, 'A');
    assert(snprintf(path, sizeof(path), "%s/kernel32.dll", runtime) > 0);
    write_one(path, 'R');
    assert(snprintf(path, sizeof(path), "%s/name..data.ini", application) > 0);
    write_one(path, 'D');
    assert(snprintf(path,sizeof(path),"%s/layout.ini",staged_nested)>0);
    write_one(path,'N');

    assert(pw_file_ps5_init(&state, application) == PW_OK);
    assert(pw_file_ps5_provider(&state, &provider) == PW_OK);
    assert(provider.open_namespace(&state, PW_FILE_RUNTIME,
                                   "kernel32.dll", &span) ==
           PW_ERR_UNSUPPORTED);
    assert(pw_file_ps5_set_runtime_directory(&state, runtime) == PW_OK);

    assert(provider.open_namespace(&state, PW_FILE_APPLICATION,
                                   "kernel32.dll", &span) == PW_OK);
    assert(span.size == 1u && ((const unsigned char *)span.bytes)[0] == 'A');
    provider.close(&state, &span);
    assert(provider.open_namespace(&state, PW_FILE_RUNTIME,
                                   "kernel32.dll", &span) == PW_OK);
    assert(span.size == 1u && ((const unsigned char *)span.bytes)[0] == 'R');
    provider.close(&state, &span);
    assert(provider.open_namespace(&state, (PwFileNamespace)99,
                                   "kernel32.dll", &span) ==
           PW_ERR_PRECONDITION);
    assert(state.opens == 2u && state.closes == 2u);

    uint32_t stream=0;unsigned char value=0;uint32_t got=0;
    assert(pw_file_ps5_stream_open(&state,"name..data.ini","rb",&stream)==PW_OK);
    assert(pw_file_ps5_stream_read(&state,stream,&value,1,&got)==PW_OK &&
           got==1u && value=='D');
    assert(pw_file_ps5_stream_close(&state,stream)==PW_OK);
    assert(pw_file_ps5_stream_open_staged(&state,"app\\SUB\\layout.ini",
        "rb",&stream)==PW_OK);
    value=0;got=0;
    assert(pw_file_ps5_stream_read(&state,stream,&value,1,&got)==PW_OK &&
           got==1u && value=='N');
    assert(pw_file_ps5_stream_close(&state,stream)==PW_OK);
    assert(pw_file_ps5_stream_open_staged(&state,"app/../kernel32.dll",
        "rb",&stream)==PW_ERR_PRECONDITION);
    assert(pw_file_ps5_stream_open_staged(&state,"/app/layout.ini",
        "rb",&stream)==PW_ERR_LIMIT);

    assert(snprintf(path, sizeof(path), "%s/kernel32.dll", application) > 0);
    assert(remove(path) == 0);
    assert(snprintf(path, sizeof(path), "%s/kernel32.dll", runtime) > 0);
    assert(remove(path) == 0);
    assert(snprintf(path, sizeof(path), "%s/name..data.ini", application) > 0);
    assert(remove(path) == 0);
    assert(snprintf(path,sizeof(path),"%s/layout.ini",staged_nested)>0);
    assert(remove(path)==0);
    assert(rmdir(staged_nested)==0);
    assert(rmdir(staged_application)==0);
    assert(rmdir(application) == 0);
    assert(rmdir(runtime) == 0);
    assert(rmdir(root) == 0);
    return 0;
}
