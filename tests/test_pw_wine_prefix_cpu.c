/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../native/pw_wine_prefix.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    assert(file && fwrite(text, 1, strlen(text), file) == strlen(text) && fclose(file) == 0);
}

static int file_is(const char *path, const char *text)
{
    char buffer[64] = {0};
    FILE *file = fopen(path, "rb");
    size_t n;

    if (!file) return 0;
    n = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    return n == strlen(text) && !memcmp(buffer, text, n);
}

int main(void)
{
    char root[] = "/tmp/pw_prefix_cpu_XXXXXX", prefix[256], system32[300], source[300], installed[340],
         temporary[350];

    assert(mkdtemp(root));
    snprintf(prefix, sizeof(prefix), "%s/prefix", root);
    snprintf(system32, sizeof(system32), "%s/drive_c/windows/system32", prefix);
    snprintf(source, sizeof(source), "%s/wow64native.dll", root);
    snprintf(installed, sizeof(installed), "%s/wow64native.dll", system32);
    snprintf(temporary, sizeof(temporary), "%s.new", installed);
    write_file(source, "native v1");

    /* a prefix Wine has not made yet: nothing to install into */
    assert(pw_wine_prefix_cpu_install(prefix, source, "wow64native.dll") == -1);
    assert(!mkdir(prefix, 0777));
    {
        char path[300];
        snprintf(path, sizeof(path), "%s/drive_c", prefix); assert(!mkdir(path, 0777));
        snprintf(path, sizeof(path), "%s/drive_c/windows", prefix); assert(!mkdir(path, 0777));
        assert(!mkdir(system32, 0777));
    }

    /* first launch copies it, the next one finds the same file */
    assert(pw_wine_prefix_cpu_install(prefix, source, "wow64native.dll") == 1);
    assert(file_is(installed, "native v1") && access(temporary, F_OK) != 0);
    assert(pw_wine_prefix_cpu_install(prefix, source, "wow64native.dll") == 0);

    /* a runtime update replaces a stale copy, also one of the same length */
    write_file(source, "native v2");
    assert(pw_wine_prefix_cpu_install(prefix, source, "wow64native.dll") == 1);
    assert(file_is(installed, "native v2"));
    write_file(source, "a longer native build");
    assert(pw_wine_prefix_cpu_install(prefix, source, "wow64native.dll") == 1);
    assert(file_is(installed, "a longer native build") && access(temporary, F_OK) != 0);

    /* refusals leave the installed file alone */
    assert(pw_wine_prefix_cpu_install(prefix, "/nonexistent/wow64native.dll", "wow64native.dll") == -1);
    assert(pw_wine_prefix_cpu_install(prefix, source, "../wow64native.dll") == -1);
    assert(pw_wine_prefix_cpu_install(NULL, source, "wow64native.dll") == -1);
    assert(pw_wine_prefix_cpu_install(prefix, NULL, "wow64native.dll") == -1);
    assert(file_is(installed, "a longer native build"));

    remove(installed); remove(source);
    rmdir(system32);
    {
        char path[300];
        snprintf(path, sizeof(path), "%s/drive_c/windows", prefix); rmdir(path);
        snprintf(path, sizeof(path), "%s/drive_c", prefix); rmdir(path);
    }
    rmdir(prefix); rmdir(root);
    printf("prefix CPU install passed: no system32 refused, first copy, unchanged, stale replaced "
           "atomically, refusals keep the installed file\n");
    return 0;
}
