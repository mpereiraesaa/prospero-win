/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * PS5 file provider for the loader's dependency resolution.
 *
 * Descriptors come from sceKernelOpen and are released with
 * sceKernelClose; the bytes are read with libc read/lseek on that same
 * descriptor, which is the combination the laboratory has already measured
 * on FW 12.02. libc opendir and getdents are unusable on the read-only
 * application image, so there is no directory scan here: modules are staged
 * under lowercase names and looked up by exact path.
 *
 * File bytes are never taken from the libc heap. It is roughly 8 MiB and
 * cannot be grown from a title, so every image goes to an anonymous mapping,
 * the measured route for anything large on this firmware.
 */
#ifndef PROSPERO_WIN_FILE_PS5_H
#define PROSPERO_WIN_FILE_PS5_H

#include "../include/prospero_win_file.h"
#include "../src/pw_wine_gate.h"

enum {
    PW_FILE_PS5_MAX_OPEN = 32,
    PW_FILE_PS5_MAX_WINE_OPEN = 32,
    PW_FILE_PS5_MAX_BYTES = 192u * 1024u * 1024u,
};

typedef struct PwFilePs5Mapping {
    void *base;
    size_t bytes;
} PwFilePs5Mapping;

typedef struct PwFilePs5 {
    char directory[PW_PATH_MAX + 1];
    char runtime_directory[PW_PATH_MAX + 1];
    char runtime_nls_directory[PW_PATH_MAX + 1];
    uint8_t runtime_configured;
    uint8_t runtime_nls_configured;
    uint32_t opens;
    uint32_t closes;
    uint32_t failures;
    uint64_t bytes_read;
    PwFilePs5Mapping mappings[PW_FILE_PS5_MAX_OPEN];
    int streams[8];
    int wine_files[PW_FILE_PS5_MAX_WINE_OPEN];
    uint64_t wine_file_sizes[PW_FILE_PS5_MAX_WINE_OPEN];
} PwFilePs5;

/* Result of the boot-time filesystem smoke test. */
typedef struct PwFilePs5Smoke {
    int open_result;
    int stat_result;
    int read_result;
    int seek_result;
    int close_result;
    long long size;
    unsigned char first_bytes[2];
    int is_pe;
} PwFilePs5Smoke;

int pw_file_ps5_init(PwFilePs5 *state, const char *directory);

/* Configures the independent, exact-name Wine/DXVK distribution root. */
int pw_file_ps5_set_runtime_directory(PwFilePs5 *state,
                                      const char *directory);
int pw_file_ps5_set_runtime_nls_directory(PwFilePs5 *state,
                                           const char *directory);

int pw_file_ps5_provider(PwFilePs5 *state, PwFileProvider *provider);
int pw_file_ps5_wine_file_service(PwFilePs5 *state,
                                  PwWineFileService *service);

/*
 * Calls sceKernelOpen, sceKernelStat, read, lseek and sceKernelClose on one
 * staged file and records each result. Per the porting playbook, a symbol
 * that is merely exported is not a working one: this runs before the loader
 * so a failing platform call is reported as itself instead of surfacing
 * later as a mysterious parse error.
 */
int pw_file_ps5_smoke(PwFilePs5 *state, const char *name,
                      PwFilePs5Smoke *out);

/* Confined CRT-stream adapters. The legacy entry reduces guest paths to a
 * basename; the staged entry accepts only a relative package path. */
int pw_file_ps5_stream_open(void *,const char *,const char *,uint32_t *);
int pw_file_ps5_stream_open_staged(void *,const char *,const char *,uint32_t *);
int pw_file_ps5_stream_close(void *,uint32_t);
int pw_file_ps5_stream_read(void *,uint32_t,void *,uint32_t,uint32_t *);
int pw_file_ps5_stream_seek(void *,uint32_t,int32_t,uint32_t,uint32_t *);

#endif
