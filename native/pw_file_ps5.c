/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_file_ps5.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif

extern int sceKernelOpen(const char *path, int flags, int mode);
extern int sceKernelClose(int descriptor);
extern int sceKernelStat(const char *path, struct stat *status);

static size_t page_round(size_t bytes)
{
    const long page = sysconf(_SC_PAGESIZE);
    const size_t granularity = page > 0 ? (size_t)page : 4096u;

    if (bytes > (size_t)-1 - granularity)
        return 0u;
    return (bytes + granularity - 1u) & ~(granularity - 1u);
}

static int join(char *out, size_t out_bytes, const char *directory,
                const char *name)
{
    const size_t directory_length = strlen(directory);
    const size_t name_length = strlen(name);

    if (directory_length + 1u + name_length + 1u > out_bytes)
        return PW_ERR_LIMIT;
    memcpy(out, directory, directory_length);
    out[directory_length] = '/';
    memcpy(out + directory_length + 1u, name, name_length);
    out[directory_length + 1u + name_length] = '\0';
    return PW_OK;
}

static PwWineFileStatus wine_errno_status(int error)
{
    if (error == EACCES || error == EPERM)
        return PW_WINE_FILE_DENIED;
    if (error == ENOENT || error == ENOTDIR)
        return PW_WINE_FILE_NOT_FOUND;
    return PW_WINE_FILE_ERROR;
}

int pw_file_ps5_init(PwFilePs5 *state, const char *directory)
{
    size_t length;

    if (!state || !directory)
        return PW_ERR_PRECONDITION;
    memset(state, 0, sizeof(*state));
    for(unsigned index=0;index<8;index++)state->streams[index]=-1;
    for(unsigned index=0;index<PW_FILE_PS5_MAX_WINE_OPEN;index++)
        state->wine_files[index]=-1;
    length = strlen(directory);
    if (length == 0u || length > PW_PATH_MAX)
        return PW_ERR_LIMIT;
    memcpy(state->directory, directory, length);
    state->directory[length] = '\0';
    return PW_OK;
}

int pw_file_ps5_set_runtime_directory(PwFilePs5 *state,
                                      const char *directory)
{
    size_t length;

    if (!state || !directory)
        return PW_ERR_PRECONDITION;
    length = strlen(directory);
    if (length == 0u || length > PW_PATH_MAX)
        return PW_ERR_LIMIT;
    memcpy(state->runtime_directory, directory, length);
    state->runtime_directory[length] = '\0';
    state->runtime_configured = 1u;
    return PW_OK;
}

int pw_file_ps5_set_runtime_nls_directory(PwFilePs5 *state,
                                           const char *directory)
{
    size_t length;

    if (!state || !directory)
        return PW_ERR_PRECONDITION;
    length = strlen(directory);
    if (length == 0u || length > PW_PATH_MAX)
        return PW_ERR_LIMIT;
    memcpy(state->runtime_nls_directory, directory, length);
    state->runtime_nls_directory[length] = '\0';
    state->runtime_nls_configured = 1u;
    return PW_OK;
}

static int stream_slot(PwFilePs5 *state, uint32_t handle, unsigned *slot)
{
    if (!state || !slot || handle < 0x0d000001u || handle > 0x0d000008u)
        return PW_ERR_PRECONDITION;
    *slot = handle - 0x0d000001u;
    return state->streams[*slot] >= 0 ? PW_OK : PW_ERR_NOT_FOUND;
}

static int guest_path(PwFilePs5 *state, const char *guest, char *out,
                      size_t capacity)
{
    const char *base;
    char lower[PW_PATH_MAX + 1];
    size_t length;

    if (!state || !guest || !out)
        return PW_ERR_PRECONDITION;
    base = strrchr(guest, '\\');
    base = base ? base + 1 : guest;
    if (!*base || strchr(base, '/') || strchr(base, '\\') ||
        !strcmp(base, ".") || !strcmp(base, ".."))
        return PW_ERR_PRECONDITION;
    length = strlen(base);
    if (length > PW_PATH_MAX)
        return PW_ERR_LIMIT;
    for (size_t index = 0; index < length; ++index) {
        const unsigned char value = (unsigned char)base[index];

        lower[index] = (char)(value >= 'A' && value <= 'Z'
                            ? value + ('a' - 'A') : value);
    }
    lower[length] = '\0';
    return join(out, capacity, state->directory, lower);
}

static int staged_path(PwFilePs5 *state, const char *relative, char *out,
                       size_t capacity)
{
    char lower[PW_PATH_MAX + 1];
    size_t length=0u,component=0u;
    if(!state || !relative || !out)return PW_ERR_PRECONDITION;
    while(length<=PW_PATH_MAX && relative[length])length++;
    if(!length)return PW_ERR_PRECONDITION;
    if(length>PW_PATH_MAX || relative[0]=='/' || relative[0]=='\\')
        return PW_ERR_LIMIT;
    for(size_t index=0;index<=length;index++) {
        unsigned char value=(unsigned char)relative[index];
        if(value=='\\')value='/';
        if(value=='/' || value=='\0') {
            size_t component_length=index-component;
            if(!component_length || (component_length==1u && lower[component]=='.') ||
               (component_length==2u && lower[component]=='.' &&
                lower[component+1u]=='.'))return PW_ERR_PRECONDITION;
            component=index+1u;
            if(value=='\0')break;
            lower[index]='/';
            continue;
        }
        if(value<0x20u || value==0x7fu || value==':' || value=='*' ||
           value=='?' || value=='"' || value=='<' || value=='>' || value=='|')
            return PW_ERR_PRECONDITION;
        lower[index]=(char)(value>='A' && value<='Z'
            ? value+('a'-'A') : value);
    }
    lower[length]='\0';
    return join(out,capacity,state->directory,lower);
}

static int stream_open_translated(PwFilePs5 *state,const char *translated,
                                  uint32_t *handle)
{
    unsigned slot=8u;
    int descriptor;
    for(unsigned index=0;index<8u;index++)if(state->streams[index]<0) {
        slot=index;break;
    }
    if(slot==8u)return PW_ERR_LIMIT;
    descriptor=sceKernelOpen(translated,O_RDONLY,0);
    if(descriptor<0)return PW_ERR_NOT_FOUND;
    state->streams[slot]=descriptor;
    *handle=0x0d000001u+slot;
    return PW_OK;
}

int pw_file_ps5_stream_open(void *opaque, const char *path, const char *mode,
                            uint32_t *handle)
{
    PwFilePs5 *state = opaque;
    char translated[2u * (PW_PATH_MAX + 1u)];
    int status;

    if (!state || !path || !mode || !handle ||
        (strcmp(mode, "r") && strcmp(mode, "rb")))
        return PW_ERR_UNSUPPORTED;
    status = guest_path(state, path, translated, sizeof(translated));
    if (status != PW_OK)
        return status;
    return stream_open_translated(state,translated,handle);
}

int pw_file_ps5_stream_open_staged(void *opaque,const char *path,
                                   const char *mode,uint32_t *handle)
{
    PwFilePs5 *state=opaque;
    char translated[2u*(PW_PATH_MAX+1u)];
    int status;
    if(!state || !path || !mode || !handle ||
       (strcmp(mode,"r") && strcmp(mode,"rb")))return PW_ERR_UNSUPPORTED;
    status=staged_path(state,path,translated,sizeof(translated));
    if(status!=PW_OK)return status;
    return stream_open_translated(state,translated,handle);
}

int pw_file_ps5_stream_close(void *opaque, uint32_t handle)
{
    PwFilePs5 *state = opaque;
    unsigned slot;
    int status = stream_slot(state, handle, &slot);
    int result;

    if (status != PW_OK)
        return status;
    result = sceKernelClose(state->streams[slot]);
    state->streams[slot] = -1;
    return result == 0 ? PW_OK : PW_ERR_STATE;
}

int pw_file_ps5_stream_read(void *opaque, uint32_t handle, void *output,
                            uint32_t bytes, uint32_t *got)
{
    PwFilePs5 *state = opaque;
    unsigned slot;
    int status = stream_slot(state, handle, &slot);
    ssize_t result;

    if (status != PW_OK || !output || !got)
        return status == PW_OK ? PW_ERR_PRECONDITION : status;
    result = read(state->streams[slot], output, bytes);
    if (result < 0)
        return PW_ERR_STATE;
    *got = (uint32_t)result;
    return PW_OK;
}

int pw_file_ps5_stream_seek(void *opaque, uint32_t handle, int32_t offset,
                            uint32_t origin, uint32_t *position)
{
    PwFilePs5 *state = opaque;
    unsigned slot;
    int status = stream_slot(state, handle, &slot);
    off_t result;

    if (status != PW_OK || !position || origin > 2u)
        return status == PW_OK ? PW_ERR_PRECONDITION : status;
    result = lseek(state->streams[slot], offset, (int)origin);
    if (result < 0 || (uint64_t)result > UINT32_MAX)
        return PW_ERR_STATE;
    *position = (uint32_t)result;
    return PW_OK;
}

static int remember(PwFilePs5 *state, void *base, size_t bytes)
{
    for (uint32_t index = 0; index < PW_FILE_PS5_MAX_OPEN; ++index) {
        if (state->mappings[index].base == NULL) {
            state->mappings[index].base = base;
            state->mappings[index].bytes = bytes;
            return PW_OK;
        }
    }
    return PW_ERR_LIMIT;
}

static size_t forget(PwFilePs5 *state, void *base)
{
    for (uint32_t index = 0; index < PW_FILE_PS5_MAX_OPEN; ++index) {
        if (state->mappings[index].base == base) {
            const size_t bytes = state->mappings[index].bytes;

            state->mappings[index].base = NULL;
            state->mappings[index].bytes = 0u;
            return bytes;
        }
    }
    return 0u;
}

/* Reads a whole staged file into a private anonymous mapping. */
static int read_file(PwFilePs5 *state, const char *path, PwFileSpan *out)
{
    struct stat status;
    int descriptor;
    size_t bytes;
    size_t mapped_bytes;
    uint8_t *buffer;
    size_t done = 0u;

    memset(out, 0, sizeof(*out));
    if (sceKernelStat(path, &status) != 0)
        return PW_ERR_NOT_FOUND;
    if (status.st_size <= 0)
        return PW_ERR_TRUNCATED;
    if ((unsigned long long)status.st_size > PW_FILE_PS5_MAX_BYTES)
        return PW_ERR_LIMIT;
    bytes = (size_t)status.st_size;
    mapped_bytes = page_round(bytes);
    if (mapped_bytes == 0u)
        return PW_ERR_OVERFLOW;

    descriptor = sceKernelOpen(path, O_RDONLY, 0);
    if (descriptor < 0)
        return PW_ERR_NOT_FOUND;
    buffer = mmap(NULL, mapped_bytes, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buffer == MAP_FAILED) {
        (void)sceKernelClose(descriptor);
        return PW_ERR_VM;
    }
    while (done < bytes) {
        const ssize_t chunk = read(descriptor, buffer + done, bytes - done);

        if (chunk > 0) {
            done += (size_t)chunk;
            continue;
        }
        if (chunk < 0 && errno == EINTR)
            continue;
        break;
    }
    (void)sceKernelClose(descriptor);
    if (done != bytes) {
        (void)munmap(buffer, mapped_bytes);
        return PW_ERR_TRUNCATED;
    }
    if (remember(state, buffer, mapped_bytes) != PW_OK) {
        (void)munmap(buffer, mapped_bytes);
        return PW_ERR_LIMIT;
    }

    out->bytes = buffer;
    out->size = bytes;
    out->handle = buffer;
    if (strlen(path) <= PW_PATH_MAX)
        memcpy(out->path, path, strlen(path) + 1u);
    ++state->opens;
    state->bytes_read += bytes;
    return PW_OK;
}

static int open_from_directory(PwFilePs5 *state, const char *directory,
                               const char *canonical_name, PwFileSpan *out)
{
    char path[2u * (PW_PATH_MAX + 1u)];
    int status;

    if (!state || !directory || !canonical_name || !out)
        return PW_ERR_PRECONDITION;
    status = join(path, sizeof(path), directory, canonical_name);
    if (status != PW_OK)
        return status;
    status = read_file(state, path, out);
    if (status != PW_OK)
        ++state->failures;
    return status;
}

static int provider_open(void *context, const char *canonical_name,
                         PwFileSpan *out)
{
    PwFilePs5 *state = context;

    if (!state)
        return PW_ERR_PRECONDITION;
    return open_from_directory(state, state->directory, canonical_name, out);
}

static int provider_open_namespace(void *context,
                                   PwFileNamespace file_namespace,
                                   const char *canonical_name, PwFileSpan *out)
{
    PwFilePs5 *state = context;

    if (!state)
        return PW_ERR_PRECONDITION;
    if (file_namespace == PW_FILE_APPLICATION)
        return open_from_directory(state, state->directory, canonical_name, out);
    if (file_namespace != PW_FILE_RUNTIME)
        return PW_ERR_PRECONDITION;
    if (!state->runtime_configured)
        return PW_ERR_UNSUPPORTED;
    return open_from_directory(state, state->runtime_directory,
                               canonical_name, out);
}

static void provider_close(void *context, PwFileSpan *span)
{
    PwFilePs5 *state = context;
    size_t bytes;

    if (!state || !span || !span->handle)
        return;
    bytes = forget(state, span->handle);
    if (bytes != 0u)
        (void)munmap(span->handle, bytes);
    span->handle = NULL;
    span->bytes = NULL;
    span->size = 0u;
    ++state->closes;
}

int pw_file_ps5_provider(PwFilePs5 *state, PwFileProvider *provider)
{
    if (!state || !provider)
        return PW_ERR_PRECONDITION;
    provider->context = state;
    provider->open = provider_open;
    provider->close = provider_close;
    provider->open_namespace = provider_open_namespace;
    return PW_OK;
}

static int wine_name_valid(const char *name)
{
    size_t length = 0u;

    if (!name || !name[0])
        return 0;
    while (length <= PW_WINE_GATE_MAX_PATH && name[length]) {
        const unsigned char value = (unsigned char)name[length];

        if (value < 0x20u || value == 0x7fu || value == '/' ||
            value == '\\' || value == ':')
            return 0;
        ++length;
    }
    return length <= PW_WINE_GATE_MAX_PATH &&
           strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

static PwWineFileStatus wine_file_try_open(PwFilePs5 *state,
                                           const char *directory,
                                           const char *name,
                                           unsigned slot, void **token,
                                           uint64_t *size)
{
    char path[2u * (PW_PATH_MAX + 1u)];
    struct stat status;
    int descriptor;

    if (!directory || !directory[0] || join(path, sizeof(path), directory,
                                            name) != PW_OK)
        return PW_WINE_FILE_ERROR;
    if (sceKernelStat(path, &status) != 0)
        return wine_errno_status(errno);
    if (!S_ISREG(status.st_mode) || status.st_size < 0)
        return PW_WINE_FILE_ERROR;
    descriptor = sceKernelOpen(path, O_RDONLY, 0);
    if (descriptor < 0)
        return wine_errno_status(errno);
    state->wine_files[slot] = descriptor;
    state->wine_file_sizes[slot] = (uint64_t)status.st_size;
    *size = (uint64_t)status.st_size;
    *token = &state->wine_files[slot];
    return PW_WINE_FILE_OK;
}

static PwWineFileStatus wine_file_open(void *context,
                                       PwFileNamespace file_namespace,
                                       const char *name, uint64_t *size,
                                       void **token)
{
    PwFilePs5 *state = context;
    unsigned slot = PW_FILE_PS5_MAX_WINE_OPEN;
    PwWineFileStatus status;

    if (!state || !size || !token || !wine_name_valid(name))
        return PW_WINE_FILE_DENIED;
    for (unsigned index = 0u; index < PW_FILE_PS5_MAX_WINE_OPEN; ++index) {
        if (state->wine_files[index] < 0) {
            slot = index;
            break;
        }
    }
    if (slot == PW_FILE_PS5_MAX_WINE_OPEN)
        return PW_WINE_FILE_ERROR;
    if (file_namespace == PW_FILE_APPLICATION)
        return wine_file_try_open(state, state->directory, name, slot,
                                  token, size);
    if (file_namespace != PW_FILE_RUNTIME || !state->runtime_configured)
        return PW_WINE_FILE_NOT_FOUND;
    if (state->runtime_nls_configured) {
        status = wine_file_try_open(state, state->runtime_nls_directory,
                                    name, slot, token, size);
        if (status == PW_WINE_FILE_OK || status == PW_WINE_FILE_DENIED)
            return status;
    }
    return wine_file_try_open(state, state->runtime_directory, name, slot,
                              token, size);
}

static int wine_file_slot(PwFilePs5 *state, void *token, unsigned *slot)
{
    if (!state || !token || !slot)
        return PW_ERR_PRECONDITION;
    for (unsigned index = 0u; index < PW_FILE_PS5_MAX_WINE_OPEN; ++index) {
        if (token == &state->wine_files[index] &&
            state->wine_files[index] >= 0) {
            *slot = index;
            return PW_OK;
        }
    }
    return PW_ERR_NOT_FOUND;
}

static PwWineFileStatus wine_file_read(void *context, void *token,
                                       uint64_t offset, void *bytes,
                                       uint32_t size, uint32_t *read_bytes)
{
    PwFilePs5 *state = context;
    unsigned slot;
    off_t result_offset;
    ssize_t result;

    if (!bytes || !read_bytes || wine_file_slot(state, token, &slot) != PW_OK ||
        offset > (uint64_t)INT64_MAX)
        return PW_WINE_FILE_ERROR;
    result_offset = lseek(state->wine_files[slot], (off_t)offset, SEEK_SET);
    if (result_offset < 0 || (uint64_t)result_offset != offset)
        return PW_WINE_FILE_ERROR;
    result = read(state->wine_files[slot], bytes, size);
    if (result < 0)
        return PW_WINE_FILE_ERROR;
    *read_bytes = (uint32_t)result;
    return PW_WINE_FILE_OK;
}

static void wine_file_close(void *context, void *token)
{
    PwFilePs5 *state = context;
    unsigned slot;

    if (wine_file_slot(state, token, &slot) != PW_OK)
        return;
    (void)sceKernelClose(state->wine_files[slot]);
    state->wine_files[slot] = -1;
    state->wine_file_sizes[slot] = 0u;
}

int pw_file_ps5_wine_file_service(PwFilePs5 *state,
                                  PwWineFileService *service)
{
    if (!state || !service)
        return PW_ERR_PRECONDITION;
    service->context = state;
    service->open = wine_file_open;
    service->read = wine_file_read;
    service->close = wine_file_close;
    return PW_OK;
}

int pw_file_ps5_smoke(PwFilePs5 *state, const char *name,
                      PwFilePs5Smoke *out)
{
    char path[2u * (PW_PATH_MAX + 1u)];
    struct stat status;
    unsigned char header[2] = {0, 0};
    unsigned char again[2] = {0, 0};
    int descriptor;
    int result;

    if (!state || !name || !out)
        return PW_ERR_PRECONDITION;
    memset(out, 0, sizeof(*out));
    out->open_result = -1;
    out->stat_result = -1;
    out->read_result = -1;
    out->seek_result = -1;
    out->close_result = -1;

    result = join(path, sizeof(path), state->directory, name);
    if (result != PW_OK)
        return result;

    out->stat_result = sceKernelStat(path, &status) == 0 ? 0 : errno;
    if (out->stat_result == 0)
        out->size = (long long)status.st_size;

    descriptor = sceKernelOpen(path, O_RDONLY, 0);
    out->open_result = descriptor >= 0 ? 0 : errno;
    if (descriptor < 0)
        return PW_ERR_NOT_FOUND;

    /* read() on a sceKernelOpen descriptor: measured, not assumed. */
    out->read_result = read(descriptor, header, sizeof(header)) ==
                       (ssize_t)sizeof(header) ? 0 : errno;
    /* lseek() must address the same descriptor namespace. */
    if (lseek(descriptor, 0, SEEK_SET) == 0 &&
        read(descriptor, again, sizeof(again)) == (ssize_t)sizeof(again) &&
        again[0] == header[0] && again[1] == header[1])
        out->seek_result = 0;
    else
        out->seek_result = errno != 0 ? errno : -1;
    out->close_result = sceKernelClose(descriptor) == 0 ? 0 : errno;

    out->first_bytes[0] = header[0];
    out->first_bytes[1] = header[1];
    out->is_pe = header[0] == 'M' && header[1] == 'Z';
    if (out->stat_result != 0 || out->read_result != 0 ||
        out->seek_result != 0 || out->close_result != 0)
        return PW_ERR_VM;
    return out->is_pe ? PW_OK : PW_ERR_NOT_PE;
}
