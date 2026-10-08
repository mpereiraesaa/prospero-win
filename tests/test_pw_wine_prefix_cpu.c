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


/* Original synthetic DLL headers; no proprietary fixture data. */
static void synthetic_dll(const char *path, int bits, unsigned char revision)
{
    unsigned char pe[512] = {0};
    FILE *file;
    pe[0] = 'M'; pe[1] = 'Z'; pe[60] = 64;
    memcpy(pe + 64, "PE\0\0", 4);
    pe[68] = bits == 32 ? 0x4c : 0x64; pe[69] = bits == 32 ? 1 : 0x86;
    pe[84] = bits == 32 ? 224 : 240; pe[87] = 0x20;
    pe[88] = 0x0b; pe[89] = bits == 32 ? 1 : 2; pe[511] = revision;
    file = fopen(path, "wb");
    assert(file && fwrite(pe, 1, sizeof(pe), file) == sizeof(pe) && !fclose(file));
}

static void synthetic_import(const char *path, const char *name)
{
    unsigned char pe[512];
    FILE *file = fopen(path, "r+b");
    assert(file && fread(pe, 1, sizeof(pe), file) == sizeof(pe));
    /* Import directory and name in the header span, no sections required. */
    pe[148] = 0; pe[149] = 2; /* SizeOfHeaders512 */
    pe[180] = 2;             /* NumberOfRvaAndSizes2 */
    pe[192] = 0x2c; pe[193] = 1; pe[196] = 40; /* ImportRVA300, size40 */
    pe[312] = 0x5e; pe[313] = 1;             /* NameRVA350 */
    assert(strlen(name) < 128); strcpy((char *)pe + 350, name);
    assert(!fseek(file, 0, SEEK_SET));
    assert(fwrite(pe, 1, sizeof(pe), file) == sizeof(pe) && !fclose(file));
}

static void fixture_u32(unsigned char *p, unsigned value)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (unsigned char)(value >> (i * 8));
}

static void synthetic_section_import(const char *path, int bits, int broken)
{
    unsigned char pe[1024] = {0};
    unsigned optional = 88, directories = bits == 32 ? 96 : 112;
    unsigned section = optional + (bits == 32 ? 224 : 240);
    synthetic_dll(path, bits, 4);
    FILE *file = fopen(path, "r+b");
    assert(file && fread(pe, 1, 512, file) == 512);
    pe[70] = 1;
    fixture_u32(pe + optional + 60, 512);
    fixture_u32(pe + optional + directories - 4, 2);
    fixture_u32(pe + optional + directories + 8, 0x1000);
    fixture_u32(pe + optional + directories + 12, broken == 1 ? 20 : 40);
    fixture_u32(pe + section + 12, 0x1000);
    fixture_u32(pe + section + 16, 512);
    fixture_u32(pe + section + 20, 512);
    fixture_u32(pe + 512 + 12, 0x1080);
    if (broken == 2) memset(pe + 640, 'a', 128);
    else strcpy((char *)pe + 640, "libc++.dll");
    assert(!fseek(file, 0, SEEK_SET));
    assert(fwrite(pe, 1, sizeof(pe), file) == sizeof(pe) && !fclose(file));
}

static void test_zink(void)
{
    char root[] = "/tmp/pw_prefix_zink_XXXXXX", prefix[256], provider[256], path[512], installed[512];
    unsigned copied;
    assert(mkdtemp(root));
    snprintf(prefix, sizeof(prefix), "%s/prefix", root); assert(!mkdir(prefix, 0700));
    snprintf(provider, sizeof(provider), "%s/provider", root); assert(!mkdir(provider, 0700));
    assert(pw_wine_prefix_zink_install(NULL, provider, PW_APP_ARCH_PE32, &copied) == PW_ERR_PRECONDITION);
    assert(pw_wine_prefix_zink_install(prefix, provider, PW_APP_ARCH_PE32, NULL) == PW_ERR_PRECONDITION);
    assert(pw_wine_prefix_zink_install(prefix, provider, 3, &copied) == PW_ERR_PRECONDITION);
    snprintf(path, sizeof(path), "%s/opengl32.dll", provider); synthetic_dll(path, 32, 1);
    assert(pw_wine_prefix_zink_install(prefix, provider, PW_APP_ARCH_PE32, &copied) == PW_ERR_NOT_FOUND);
    assert(copied == 0);
    snprintf(path, sizeof(path), "%s/libgallium_wgl.dll", provider); synthetic_dll(path, 32, 1);
    snprintf(path, sizeof(path), "%s/opengl32.dll", provider); synthetic_import(path, "libc++.dll");
    assert(pw_wine_prefix_zink_install(prefix, provider, PW_APP_ARCH_PE32, &copied) == PW_ERR_NOT_FOUND && !copied);
    snprintf(path, sizeof(path), "%s/libc++.dll", provider); synthetic_dll(path, 64, 1);
    /* Every companion is validated before installing the first DLL. */
    assert(pw_wine_prefix_zink_install(prefix, provider, PW_APP_ARCH_PE32, &copied) == PW_ERR_UNSUPPORTED);
    snprintf(installed, sizeof(installed), "%s/drive_c/windows/syswow64/opengl32.dll", prefix);
    assert(copied == 0 && access(installed, F_OK));
    synthetic_dll(path, 32, 1);
    assert(pw_wine_prefix_zink_install(prefix, provider, PW_APP_ARCH_PE32, &copied) == PW_OK && copied == 3);
    assert(!access(installed, F_OK));
    assert(pw_wine_prefix_zink_install(prefix, provider, PW_APP_ARCH_PE32, &copied) == PW_OK && copied == 0);
    synthetic_dll(path, 32, 2);
    assert(pw_wine_prefix_zink_install(prefix, provider, PW_APP_ARCH_PE32, &copied) == PW_OK && copied == 1);
    /* Malformed, truncated, oversize and unsafe names fail without replacing. */
    write_file(path, "MZ");
    assert(pw_wine_prefix_zink_install(prefix, provider, PW_APP_ARCH_PE32, &copied) == PW_ERR_TRUNCATED && !copied);
    synthetic_dll(path, 32, 2);
    FILE *file = fopen(path, "r+b"); assert(file);
    assert(!ftruncate(fileno(file), (128u << 20) + 1u) && !fclose(file));
    assert(pw_wine_prefix_zink_install(prefix, provider, PW_APP_ARCH_PE32, &copied) == PW_ERR_LIMIT && !copied);
    synthetic_dll(path, 32, 2);
    snprintf(path, sizeof(path), "%s/bad name.dll", provider); synthetic_dll(path, 32, 1);
    assert(pw_wine_prefix_zink_install(prefix, provider, PW_APP_ARCH_PE32, &copied) == PW_ERR_MALFORMED && !copied);
    assert(!remove(path));
    snprintf(path, sizeof(path), "%s/OPENGL32.DLL", provider); synthetic_dll(path, 32, 1);
    assert(pw_wine_prefix_zink_install(prefix, provider, PW_APP_ARCH_PE32, &copied) == PW_ERR_MALFORMED && !copied);
    assert(!remove(path));
    for (unsigned i = 0; i < 30; i++) {
        snprintf(path, sizeof(path), "%s/extra-%02u.dll", provider, i); synthetic_dll(path, 32, 1);
    }
    assert(pw_wine_prefix_zink_install(prefix, provider, PW_APP_ARCH_PE32, &copied) == PW_ERR_LIMIT && !copied);
    for (unsigned i = 0; i < 30; i++) {
        snprintf(path, sizeof(path), "%s/extra-%02u.dll", provider, i); assert(!remove(path));
    }
    /* 64-bit providers target system32, without changing the 32-bit DLLs. */
    const char *names[] = { "opengl32.dll", "libgallium_wgl.dll", "libc++.dll" };
    for (unsigned i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), "%s/%s", provider, names[i]); synthetic_dll(path, 64, 4);
    }
    assert(pw_wine_prefix_zink_install(prefix, provider, PW_APP_ARCH_PE64, &copied) == PW_OK && copied == 3);
    FILE *old = fopen(installed, "rb"); unsigned char pe[512]; assert(old);
    assert(fread(pe, 1, sizeof(pe), old) == sizeof(pe) && !fclose(old)); assert(pe[68] == 0x4c && pe[511] == 1);
    snprintf(path, sizeof(path), "%s/opengl32.dll", provider);
    synthetic_section_import(path, 64, 0);
    assert(pw_wine_prefix_zink_install(prefix, provider, PW_APP_ARCH_PE64, &copied) == PW_OK && copied == 1);
    for (int broken = 1; broken <= 2; broken++) {
        synthetic_section_import(path, 64, broken);
        assert(pw_wine_prefix_zink_install(prefix, provider, PW_APP_ARCH_PE64, &copied) == PW_ERR_MALFORMED && !copied);
    }
    synthetic_section_import(path, 64, 0);
    assert(pw_wine_prefix_zink_install(prefix, provider, PW_APP_ARCH_PE64, &copied) == PW_OK && !copied);
    /* Rename failure never deletes an existing destination or temporary data. */
    snprintf(path, sizeof(path), "%s/drive_c/windows/system32/libc++.dll", prefix); assert(!remove(path));
    assert(!mkdir(path, 0700));
    assert(pw_wine_prefix_zink_install(prefix, provider, PW_APP_ARCH_PE64, &copied) == PW_ERR_STATE);
    char temporary[550]; snprintf(temporary, sizeof(temporary), "%s.new", path); assert(access(temporary, F_OK));
    assert(!rmdir(path));
    for (unsigned i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), "%s/%s", provider, names[i]); assert(!remove(path));
        snprintf(path, sizeof(path), "%s/drive_c/windows/syswow64/%s", prefix, names[i]); assert(!remove(path));
        snprintf(path, sizeof(path), "%s/drive_c/windows/system32/%s", prefix, names[i]); remove(path);
    }
    snprintf(path, sizeof(path), "%s/drive_c/windows/syswow64", prefix); assert(!rmdir(path));
    snprintf(path, sizeof(path), "%s/drive_c/windows/system32", prefix); assert(!rmdir(path));
    snprintf(path, sizeof(path), "%s/drive_c/windows", prefix); assert(!rmdir(path));
    snprintf(path, sizeof(path), "%s/drive_c", prefix); assert(!rmdir(path));
    assert(!rmdir(prefix) && !rmdir(provider) && !rmdir(root));
}

int main(void)
{
    test_zink();
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
