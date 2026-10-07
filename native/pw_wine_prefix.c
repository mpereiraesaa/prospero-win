/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_wine_prefix.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <stdlib.h>

/* mkdir -p for everything after path[base], which exists. */
static int make_folders(char *path, size_t base)
{
    for (size_t i = base + 1; path[i]; i++) {
        if (path[i] != '/') continue;
        path[i] = 0;
        if (mkdir(path, 0777) && errno != EEXIST) { path[i] = '/'; return -1; }
        path[i] = '/';
    }
    return mkdir(path, 0777) && errno != EEXIST ? -1 : 0;
}

int pw_wine_prefix_temp_create(const char *prefix, PwPrefixTemp *temp)
{
    char path[1024], line[1024];
    FILE *file;

    pw_prefix_temp_init(temp);
    if (snprintf(path, sizeof(path), "%s/user.reg", prefix) >= (int)sizeof(path)) return -1;
    if ((file = fopen(path, "r"))) {
        while (fgets(line, sizeof(line), file)) {
            size_t length = strcspn(line, "\r\n");
            int whole = line[length] != 0 || feof(file);
            pw_prefix_temp_line(temp, line, length);
            /* skip the rest of a line longer than the buffer (hex data) */
            while (!whole && fgets(line, sizeof(line), file))
                whole = line[strcspn(line, "\r\n")] != 0;
        }
        fclose(file);
    }
    for (size_t i = 0; i < temp->count; i++) {
        int base = snprintf(path, sizeof(path), "%s/drive_c", prefix);
        if (base < 0 || snprintf(path + base, sizeof(path) - (size_t)base, "/%s", temp->folders[i]) >=
            (int)(sizeof(path) - (size_t)base)) return -1;
        if (make_folders(path, (size_t)base)) return -1;
    }
    return (int)temp->count;
}

/* Whole small files; NULL when unreadable or larger than limit. */
static unsigned char *read_file(const char *path, size_t limit, size_t *size)
{
    FILE *file = fopen(path, "rb");
    unsigned char *data;
    long length;

    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 || (size_t)length > limit ||
        fseek(file, 0, SEEK_SET) || !(data = malloc(length ? (size_t)length : 1))) {
        fclose(file);
        return NULL;
    }
    *size = fread(data, 1, (size_t)length, file);
    fclose(file);
    if (*size != (size_t)length) { free(data); return NULL; }
    return data;
}

int pw_wine_prefix_cpu_install(const char *prefix, const char *source, const char *name)
{
    enum { CPU_DLL_LIMIT = 16u << 20 };
    char path[1024], temporary[1056];
    unsigned char *wanted, *present;
    size_t wanted_size, present_size;
    int result = -1;
    FILE *file;

    if (!prefix || !source || !name || strchr(name, '/') ||
        snprintf(path, sizeof(path), "%s/drive_c/windows/system32/%s", prefix, name) >= (int)sizeof(path) ||
        snprintf(temporary, sizeof(temporary), "%s.new", path) >= (int)sizeof(temporary) ||
        !(wanted = read_file(source, CPU_DLL_LIMIT, &wanted_size)))
        return -1;
    if ((present = read_file(path, CPU_DLL_LIMIT, &present_size))) {
        int same = present_size == wanted_size && !memcmp(present, wanted, wanted_size);
        free(present);
        if (same) { free(wanted); return 0; }
    }
    if ((file = fopen(temporary, "wb"))) {
        int written = fwrite(wanted, 1, wanted_size, file) == wanted_size;
        if (fclose(file) == 0 && written && rename(temporary, path) == 0) result = 1;
        else remove(temporary);
    }
    free(wanted);
    return result;
}
