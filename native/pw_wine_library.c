/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_wine_library.h"
#include "../src/pw_profile_catalog.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#ifndef __linux__
int getdents(int fd, char *buffer, int size);
#endif

static const char profile_suffix[] = ".profile";

/* The whole file, up to capacity bytes; its length, or -1. */
static ssize_t read_file(const char *path, uint8_t *buffer, size_t capacity)
{
    size_t used = 0;
    int fd = open(path, O_RDONLY);

    if (fd < 0) return -1;
    for (;;) {
        ssize_t got = read(fd, buffer + used, capacity - used);
        if (got < 0) { close(fd); return -1; }
        if (!got) break;
        used += (size_t)got;
        if (used == capacity) {       /* larger than any valid file */
            uint8_t extra;
            if (read(fd, &extra, 1) > 0) { close(fd); return (ssize_t)capacity + 1; }
            break;
        }
    }
    close(fd);
    return (ssize_t)used;
}

static int profile_name(const char *name, size_t length)
{
    size_t suffix = sizeof(profile_suffix) - 1;

    if (length <= suffix || length >= PW_WINE_LIBRARY_NAME ||
        memcmp(name + length - suffix, profile_suffix, suffix))
        return 0;
    for (size_t i = 0; i < length - suffix; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
    }
    return 1;
}

static void add_name(const char *name, size_t length, void *context)
{
    PwWineLibrary *library = context;

    if (!profile_name(name, length) || library->count >= PW_WINE_LIBRARY_MAX) return;
    for (uint32_t i = 0; i < library->count; i++)
        if (!strncmp(library->entries[i].file, name, length) && !library->entries[i].file[length])
            return;
    memcpy(library->entries[library->count].file, name, length);
    library->entries[library->count].file[length] = 0;
    library->count++;
}

int pw_wine_library_dirents(const uint8_t *buffer, size_t length,
                            void (*found)(const char *name, size_t length, void *context),
                            void *context)
{
    size_t offset = 0;
    int records = 0;

    if (!buffer || !found) return -1;
    while (offset < length) {
        uint16_t reclen;
        uint8_t type, namlen;

        if (length - offset < 8) return -1;
        memcpy(&reclen, buffer + offset + 4, 2);
        type = buffer[offset + 6];
        namlen = buffer[offset + 7];
        if (reclen < 8 || reclen > length - offset || (size_t)namlen + 8 > reclen) return -1;
        /* DT_UNKNOWN 0 and DT_REG 8: a file system may not fill the type. */
        if (namlen && (type == 0 || type == 8))
            found((const char *)buffer + offset + 8, namlen, context);
        offset += reclen;
        records++;
    }
    return records;
}

/* Names through libc's readdir; 0, or -1 with errno. */
static int scan_readdir(PwWineLibrary *library, const char *directory)
{
    DIR *dir = opendir(directory);
    struct dirent *entry;

    if (!dir) return -1;
    while ((entry = readdir(dir))) add_name(entry->d_name, strlen(entry->d_name), library);
    closedir(dir);
    return 0;
}

#ifndef __linux__
/* Names through getdents; 0, or -1 with errno (EINVAL for a malformed
 * buffer). The buffer is a whole directory block: libc's readdir reads with
 * one at least a page (16 KiB on the console), and /data's directories use
 * 64 KiB blocks, while a 4 KiB buffer got EINVAL there. */
static int scan_getdents(PwWineLibrary *library, const char *directory)
{
    static uint8_t buffer[64 * 1024];
    int fd = open(directory, O_RDONLY | O_DIRECTORY), got, error;

    if (fd < 0) return -1;
    while ((got = getdents(fd, (char *)buffer, sizeof(buffer))) > 0)
        if (pw_wine_library_dirents(buffer, (size_t)got, add_name, library) < 0) {
            errno = EINVAL;
            got = -1;
            break;
        }
    error = errno;
    close(fd);
    errno = error;
    return got < 0 ? -1 : 0;
}
#endif

/* List the directory's names into the library; 0, or -1. readdir lists
 * /data on the console (measured); getdents is the console's second try.
 * scan_error keeps the first failure's errno for the log. */
static int scan(PwWineLibrary *library, const char *directory)
{
    if (scan_readdir(library, directory) == 0) return 0;
    library->scan_error = errno;
#ifndef __linux__
    library->count = 0;
    if (scan_getdents(library, directory) == 0) return 0;
#endif
    return -1;
}

static int by_file(const void *a, const void *b)
{
    return strcmp(((const PwWineLibraryEntry *)a)->file, ((const PwWineLibraryEntry *)b)->file);
}

int pw_wine_library_load(PwWineLibrary *library, const char *root)
{
    static uint8_t text[PW_APP_PROFILE_MAX_BYTES + 1];
    char directory[PW_WINE_LIBRARY_PATH], path[PW_WINE_LIBRARY_PATH + PW_WINE_LIBRARY_NAME];
    ssize_t length;

    if (!library || !root) return PW_ERR_PRECONDITION;
    memset(library, 0, sizeof(*library));
    if (snprintf(directory, sizeof(directory), "%s/profiles", root) >= (int)sizeof(directory))
        return PW_ERR_LIMIT;
    /* An index says exactly what is offered; without one the directory is
     * listed. */
    (void)snprintf(path, sizeof(path), "%s/profiles.lst", directory);  /* always fits */
    length = read_file(path, text, sizeof(text) - 1);
    if (length >= 0) {
        PwProfileCatalog catalog;
        if (length >= (ssize_t)sizeof(text) - 1 ||
            pw_profile_catalog_parse(text, (size_t)length, &catalog) != PW_OK)
            return PW_ERR_NOT_FOUND;
        for (uint32_t i = 0; i < catalog.count; i++)
            add_name(catalog.names[i], strlen(catalog.names[i]), library);
        library->listed_by = PW_WINE_LIBRARY_INDEXED;
    } else if (scan(library, directory) == 0) {
        library->listed_by = PW_WINE_LIBRARY_SCANNED;
    } else {
        return PW_ERR_NOT_FOUND;
    }
    qsort(library->entries, library->count, sizeof(library->entries[0]), by_file);
    for (uint32_t i = 0; i < library->count; i++) {
        PwWineLibraryEntry *entry = &library->entries[i];
        if (snprintf(path, sizeof(path), "%s/%s", directory, entry->file) >= (int)sizeof(path)) {
            entry->status = PW_ERR_LIMIT;
            continue;
        }
        length = read_file(path, text, sizeof(text) - 1);
        if (length < 0) entry->status = PW_ERR_NOT_FOUND;
        else if (length >= (ssize_t)sizeof(text) - 1) entry->status = PW_ERR_LIMIT;
        else entry->status = pw_game_profile_parse(text, (size_t)length, &entry->profile);
    }
    return PW_OK;
}

const PwGameProfile *pw_wine_library_find(const PwWineLibrary *library, const char *id)
{
    if (!library || !id) return NULL;
    for (uint32_t i = 0; i < library->count; i++)
        if (library->entries[i].status == PW_OK && !strcmp(library->entries[i].profile.app.id, id))
            return &library->entries[i].profile;
    return NULL;
}

int pw_wine_library_input(const PwGameProfile *profile, const char *root, PwGameInput *input)
{
    static uint8_t text[PW_APP_PROFILE_MAX_BYTES + 1];
    char path[PW_WINE_LIBRARY_PATH + PW_WINE_LIBRARY_NAME];
    int status = PW_OK;

    if (!profile || !root || !input) return PW_ERR_PRECONDITION;
    pw_game_input_init(input);
    if (profile->input.preset[0]) {
        ssize_t length;
        if (snprintf(path, sizeof(path), "%s/input/%s.input", root, profile->input.preset) >=
            (int)sizeof(path))
            status = PW_ERR_LIMIT;
        else if ((length = read_file(path, text, sizeof(text) - 1)) < 0)
            status = PW_ERR_NOT_FOUND;
        else if (length >= (ssize_t)sizeof(text) - 1)
            status = PW_ERR_LIMIT;
        else
            status = pw_game_input_parse(text, (size_t)length, input);
    }
    pw_game_input_overlay(input, &profile->input);
    return status;
}
