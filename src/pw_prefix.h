/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PROSPERO_WIN_PW_PREFIX_H
#define PROSPERO_WIN_PW_PREFIX_H

#include "../include/prospero_win.h"
#include <stddef.h>
#include <stdint.h>

enum {
    PW_PREFIX_ID_CAPACITY = 65,
    PW_PREFIX_PATH_CAPACITY = 512,
    PW_PREFIX_HIVE_COUNT = 3,
};

typedef enum PwPrefixHive {
    PW_PREFIX_HIVE_SYSTEM = 0,
    PW_PREFIX_HIVE_USER = 1,
    PW_PREFIX_HIVE_USERDEF = 2,
} PwPrefixHive;

/* The platform adapter supplies recursive, idempotent directory creation.
 * File reads and atomic registry writes remain owned by the selected storage
 * backend and are not hidden behind native host filesystem calls here. */
typedef struct PwPrefixIo {
    void *context;
    int (*make_directories)(void *context, const char *path);
} PwPrefixIo;

typedef struct PwPrefixService {
    PwPrefixIo io;
    char storage_root[PW_PREFIX_PATH_CAPACITY];
} PwPrefixService;

typedef struct PwPrefixLayout {
    char id[PW_PREFIX_ID_CAPACITY];
    char root[PW_PREFIX_PATH_CAPACITY];
    char drive_c[PW_PREFIX_PATH_CAPACITY];
    char windows[PW_PREFIX_PATH_CAPACITY];
    char system32[PW_PREFIX_PATH_CAPACITY];
    char system[PW_PREFIX_PATH_CAPACITY];
    char program_files[PW_PREFIX_PATH_CAPACITY];
    char program_files_x86[PW_PREFIX_PATH_CAPACITY];
    char users[PW_PREFIX_PATH_CAPACITY];
    char user_home[PW_PREFIX_PATH_CAPACITY];
    char temp[PW_PREFIX_PATH_CAPACITY];
    char hives[PW_PREFIX_HIVE_COUNT][PW_PREFIX_PATH_CAPACITY];
} PwPrefixLayout;

/* storage_root must be an absolute native path without parent traversal. */
int pw_prefix_service_init(PwPrefixService *service, const char *storage_root,
                           const PwPrefixIo *io);

/* Opens or creates one persistent prefix. Directory creation is idempotent;
 * on failure the caller's layout remains unchanged and a retry is safe. */
int pw_prefix_open(PwPrefixService *service, const char *id,
                   PwPrefixLayout *layout);

#endif
