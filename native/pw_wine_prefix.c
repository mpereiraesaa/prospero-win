/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_wine_prefix.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

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
