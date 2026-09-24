/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Check a built app.profile against the exact files in its staged directory. */
#include "../src/pw_app_profile.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int fail(const char *reason)
{
    fprintf(stderr, "profile stage validation failed: %s\n", reason);
    return 1;
}

int main(int argc, char **argv)
{
    uint8_t bytes[PW_APP_PROFILE_MAX_BYTES];
    PwAppProfile profile;
    char staged_name[PW_APP_PATH_CAPACITY];
    char staged_path[4096];
    struct stat status;
    FILE *file;
    size_t length;
    size_t directory_length;
    size_t name_length;
    int separator;
    int read_error;
    int too_large;
    int close_error;

    if (argc != 3)
        return fail("usage: validate_profile_stage PROFILE STAGE_DIR");
    file = fopen(argv[1], "rb");
    if (!file)
        return fail("cannot open profile");
    length = fread(bytes, 1u, sizeof(bytes), file);
    read_error = ferror(file);
    too_large = fgetc(file) != EOF;
    close_error = fclose(file) != 0;
    if (read_error || too_large || close_error)
        return fail("profile read failed or exceeds the size limit");
    if (pw_app_profile_parse(bytes, length, &profile) != PW_OK)
        return fail("profile is malformed or unsupported");
    if (pw_app_profile_stage_name(&profile, staged_name,
                                  sizeof(staged_name)) != PW_OK)
        return fail("profile executable has no valid staged name");

    directory_length = strlen(argv[2]);
    name_length = strlen(staged_name);
    separator = directory_length != 0u && argv[2][directory_length - 1u] != '/';
    if (directory_length + (size_t)separator + name_length + 1u >
        sizeof(staged_path))
        return fail("staged path exceeds the host path buffer");
    memcpy(staged_path, argv[2], directory_length);
    if (separator)
        staged_path[directory_length++] = '/';
    memcpy(staged_path + directory_length, staged_name, name_length + 1u);
    if (stat(staged_path, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size <= 0) {
        fprintf(stderr, "profile stage validation failed: %s is not a staged nonempty file\n",
                staged_name);
        return 1;
    }

    printf("profile stage ready: id=%s executable=%s bytes=%lld\n",
           profile.id, staged_name, (long long)status.st_size);
    return 0;
}
