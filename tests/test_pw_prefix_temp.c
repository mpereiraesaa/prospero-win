/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../native/pw_wine_prefix.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void feed(PwPrefixTemp *t, const char *text)
{
    while (*text) {
        size_t n = strcspn(text, "\n");
        pw_prefix_temp_line(t, text, n);
        text += n + (text[n] == '\n');
    }
}

static int is_dir(const char *path)
{
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}

int main(void)
{
    PwPrefixTemp t;

    /* A PC-made prefix: its TEMP and TMP name the PC's user. */
    pw_prefix_temp_init(&t);
    assert(t.count == 1 && !strcmp(t.folders[0], "windows/temp"));
    feed(&t, "[Software\\\\Wine] 1\n\"TEMP\"=\"C:\\\\elsewhere\"\n"
             "[Environment] 1790438767\n#time=1dd4dd0f06f8450\n"
             "\"TEMP\"=\"C:\\\\users\\\\manuel\\\\AppData\\\\Local\\\\Temp\"\n"
             "\"TMP\"=\"C:\\\\users\\\\manuel\\\\AppData\\\\Local\\\\Temp\"\n"
             "[Keyboard Layout\\\\Preload] 1\n\"TMP\"=\"C:\\\\other\"\n");
    assert(t.count == 2 && !strcmp(t.folders[1], "users/manuel/AppData/Local/Temp"));

    /* Case, a trailing backslash, and two different folders. */
    pw_prefix_temp_init(&t);
    feed(&t, "[environment]\n\"Temp\"=\"c:\\\\Temp\\\\\"\n\"tmp\"=\"C:\\\\users\\\\x\\\\tmp\"\n");
    assert(t.count == 3 && !strcmp(t.folders[1], "Temp") && !strcmp(t.folders[2], "users/x/tmp"));

    /* Refused: unexpanded variables, other drives, escapes out of drive_c,
     * odd escaping, an unterminated value and C:\ alone. */
    pw_prefix_temp_init(&t);
    feed(&t, "[Environment]\n"
             "\"TEMP\"=str(2):\"%USERPROFILE%\\\\AppData\\\\Local\\\\Temp\"\n"
             "\"TEMP\"=\"%TMP%\"\n"
             "\"TEMP\"=\"D:\\\\temp\"\n"
             "\"TEMP\"=\"C:\\\\users\\\\..\\\\..\\\\etc\"\n"
             "\"TEMP\"=\"C:\\\\users\\\\.\\\\x\"\n"
             "\"TEMP\"=\"C:\\\\a\\\\\\\\b\"\n"
             "\"TEMP\"=\"C:\\\\a\\\\..\"\n"
             "\"TEMP\"=\"C:\\\\a\\b\"\n"
             "\"TEMP\"=\"C:\\\\unterminated\n"
             "\"TEMP\"=\"C:\\\\\"\n"
             "\"TEMPDIR\"=\"C:\\\\no\"\n");
    assert(t.count == 1);

    /* On disk: folders created under drive_c, a long hex line skipped. */
    char root[] = "/tmp/pw_prefix_temp_XXXXXX", path[512];
    assert(mkdtemp(root));
    snprintf(path, sizeof(path), "%s/drive_c", root);
    assert(!mkdir(path, 0777));
    assert(pw_wine_prefix_temp_create(root, &t) == 1);                 /* no user.reg */
    snprintf(path, sizeof(path), "%s/drive_c/windows/temp", root);
    assert(is_dir(path));
    snprintf(path, sizeof(path), "%s/user.reg", root);
    FILE *reg = fopen(path, "w");
    assert(reg);
    fprintf(reg, "WINE REGISTRY Version 2\n[Environment] 1\n\"Blob\"=hex:");
    for (int i = 0; i < 5000; i++) fprintf(reg, "00,");
    fprintf(reg, "00\n\"TEMP\"=\"C:\\\\users\\\\manuel\\\\AppData\\\\Local\\\\Temp\"\n");
    fclose(reg);
    assert(pw_wine_prefix_temp_create(root, &t) == 2);
    snprintf(path, sizeof(path), "%s/drive_c/users/manuel/AppData/Local/Temp", root);
    assert(is_dir(path));
    assert(pw_wine_prefix_temp_create(root, &t) == 2);                 /* again: all there */

    char command[600];
    snprintf(command, sizeof(command), "rm -rf %s", root);
    assert(!system(command));
    printf("prefix temp passed: the registry's TEMP and TMP, windows/temp, refused paths, "
           "folders created once, long lines skipped\n");
    return 0;
}
