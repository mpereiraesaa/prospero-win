/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_wine_prefix.h"
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <strings.h>
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


enum { ZINK_DLL_LIMIT = 128u << 20, ZINK_DLL_COUNT = 32, ZINK_DLL_NAME = 128 };

static unsigned pe_u16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }
static unsigned pe_u32(const unsigned char *p)
{
    return pe_u16(p) | pe_u16(p + 2) << 16;
}

/* Bounded architecture check, not a replacement for Wine's PE loader. */
static int zink_dll_validate(FILE *file, PwAppArchitecture architecture, size_t *size)
{
    unsigned char dos[64], pe[26];
    struct stat st;
    unsigned offset;
    if (fstat(fileno(file), &st) || !S_ISREG(st.st_mode) || st.st_size < 0)
        return PW_ERR_STATE;
    if ((uint64_t)st.st_size > ZINK_DLL_LIMIT) return PW_ERR_LIMIT;
    *size = (size_t)st.st_size;
    if (*size < sizeof(dos) || fread(dos, 1, sizeof(dos), file) != sizeof(dos))
        return PW_ERR_TRUNCATED;
    if (memcmp(dos, "MZ", 2)) return PW_ERR_NOT_PE;
    offset = pe_u32(dos + 60);
    if (offset < sizeof(dos) || offset > 4096 || offset > *size || sizeof(pe) > *size - offset)
        return PW_ERR_TRUNCATED;
    if (fseek(file, offset, SEEK_SET) || fread(pe, 1, sizeof(pe), file) != sizeof(pe))
        return PW_ERR_STATE;
    if (memcmp(pe, "PE\0\0", 4)) return PW_ERR_NOT_PE;
    if (pe_u16(pe + 4) != (architecture == PW_APP_ARCH_PE32 ? 0x14cu : 0x8664u) ||
        pe_u16(pe + 24) != (architecture == PW_APP_ARCH_PE32 ? 0x10bu : 0x20bu))
        return PW_ERR_UNSUPPORTED;
    if (!(pe_u16(pe + 22) & 0x2000u) ||
        pe_u16(pe + 20) < (architecture == PW_APP_ARCH_PE32 ? 96u : 112u) ||
        pe_u16(pe + 20) > *size - offset - 24u)
        return PW_ERR_MALFORMED;
    return fseek(file, 0, SEEK_SET) ? PW_ERR_STATE : PW_OK;
}

/* Import validation is limited to the provider/runtime companions the package
 * supports. Windows system DLL imports are resolved by Wine, not this directory. */
struct zink_section { unsigned rva, raw, bytes; };
static int zink_rva_byte(FILE *file, size_t size, unsigned headers,
                         const struct zink_section *sections, unsigned count,
                         unsigned rva, unsigned char *out)
{
    size_t offset = rva;
    int mapped = rva < headers;
    for (unsigned i = 0; !mapped && i < count; i++) {
        if (rva < sections[i].rva || rva - sections[i].rva >= sections[i].bytes) continue;
        offset = (size_t)sections[i].raw + (rva - sections[i].rva);
        mapped = 1;
    }
    return mapped && offset < size && offset <= LONG_MAX &&
           !fseek(file, (long)offset, SEEK_SET) && fread(out, 1, 1, file) == 1;
}

static int zink_imports(FILE *file, size_t size, PwAppArchitecture architecture,
                        char names[][ZINK_DLL_NAME], unsigned dll_count)
{
    unsigned char dos[64], coff[24], optional[240], section[40], descriptor[20];
    struct zink_section sections[96];
    unsigned pe, count, opt_bytes, headers, directory_offset, imports, import_bytes;
    const char *companions[] = { "libgallium_wgl.dll", "libc++.dll", "libunwind.dll", "libwinpthread-1.dll" };
    if (fseek(file, 0, SEEK_SET) || fread(dos, 1, sizeof(dos), file) != sizeof(dos)) return PW_ERR_STATE;
    pe = pe_u32(dos + 60);
    if (fseek(file, pe, SEEK_SET) || fread(coff, 1, sizeof(coff), file) != sizeof(coff)) return PW_ERR_STATE;
    count = pe_u16(coff + 6); opt_bytes = pe_u16(coff + 20);
    directory_offset = architecture == PW_APP_ARCH_PE32 ? 96 : 112;
    if (count > 96 || opt_bytes > sizeof(optional)) return PW_ERR_LIMIT;
    if (opt_bytes < directory_offset || opt_bytes > size - pe - 24u ||
        fread(optional, 1, opt_bytes, file) != opt_bytes) return PW_ERR_MALFORMED;
    if (pe_u32(optional + directory_offset - 4) < 2) return PW_OK;
    if (opt_bytes < directory_offset + 16) return PW_ERR_MALFORMED;
    headers = pe_u32(optional + 60);
    imports = pe_u32(optional + directory_offset + 8);
    import_bytes = pe_u32(optional + directory_offset + 12);
    if (!imports && !import_bytes) return PW_OK;
    if (!imports || import_bytes < 20 || import_bytes > 1024u * 20u || imports > UINT32_MAX - import_bytes ||
        headers > size || (size_t)count * sizeof(section) > size - pe - 24u - opt_bytes)
        return PW_ERR_MALFORMED;
    for (unsigned i = 0; i < count; i++) {
        if (fread(section, 1, sizeof(section), file) != sizeof(section)) return PW_ERR_STATE;
        sections[i] = (struct zink_section){ pe_u32(section + 12), pe_u32(section + 20), pe_u32(section + 16) };
        if (sections[i].raw > size || sections[i].bytes > size - sections[i].raw)
            return PW_ERR_MALFORMED;
    }
    for (unsigned at = 0; at + 20 <= import_bytes; at += 20) {
        int empty = 1;
        for (unsigned i = 0; i < 20; i++) {
            if (!zink_rva_byte(file, size, headers, sections, count, imports + at + i, descriptor + i))
                return PW_ERR_MALFORMED;
            if (descriptor[i]) empty = 0;
        }
        if (empty) return PW_OK;
        unsigned name_rva = pe_u32(descriptor + 12);
        char name[ZINK_DLL_NAME]; unsigned length;
        if (!name_rva || name_rva > UINT32_MAX - sizeof(name)) return PW_ERR_MALFORMED;
        for (length = 0; length < sizeof(name); length++) {
            if (!zink_rva_byte(file, size, headers, sections, count, name_rva + length,
                               (unsigned char *)name + length)) return PW_ERR_MALFORMED;
            if (!name[length]) break;
        }
        if (!length || length == sizeof(name)) return PW_ERR_MALFORMED;
        for (unsigned k = 0; k < sizeof(companions) / sizeof(companions[0]); k++) {
            if (strcasecmp(name, companions[k])) continue;
            unsigned i;
            for (i = 0; i < dll_count && strcasecmp(names[i], name); i++) {}
            if (i == dll_count) return PW_ERR_NOT_FOUND;
        }
    }
    return PW_ERR_MALFORMED; /* No bounded terminating import descriptor. */
}

static int zink_copy(FILE *source, size_t size, const char *path)
{
    unsigned char wanted[8192], present[8192];
    char temporary[1056];
    FILE *current = fopen(path, "rb"), *out;
    size_t remaining = size;
    int same = current != NULL;
    if (current) {
        while (remaining && same) {
            size_t n = remaining < sizeof(wanted) ? remaining : sizeof(wanted);
            same = fread(wanted, 1, n, source) == n && fread(present, 1, n, current) == n &&
                   !memcmp(wanted, present, n);
            remaining -= n;
        }
        same = same && fgetc(current) == EOF && !ferror(current);
        fclose(current);
        if (fseek(source, 0, SEEK_SET)) return PW_ERR_STATE;
        if (same) return 0;
    }
    if (snprintf(temporary, sizeof(temporary), "%s.new", path) >= (int)sizeof(temporary) ||
        !(out = fopen(temporary, "wb"))) return PW_ERR_STATE;
    remaining = size;
    while (remaining) {
        size_t n = remaining < sizeof(wanted) ? remaining : sizeof(wanted);
        if (fread(wanted, 1, n, source) != n || fwrite(wanted, 1, n, out) != n) break;
        remaining -= n;
    }
    int failed = remaining != 0 || ferror(source);
    if (fclose(out)) failed = 1;
    if (!failed && !rename(temporary, path)) return 1;
    remove(temporary);
    return PW_ERR_STATE;
}

int pw_wine_prefix_zink_install(const char *prefix, const char *provider,
                                PwAppArchitecture architecture, unsigned *copied)
{
    char names[ZINK_DLL_COUNT][ZINK_DLL_NAME], source[1024], destination[1024], folder[1024];
    unsigned count = 0, core = 0;
    int status = PW_OK;
    DIR *directory;
    struct dirent *entry;
    if (!prefix || !provider || !copied ||
        (architecture != PW_APP_ARCH_PE32 && architecture != PW_APP_ARCH_PE64))
        return PW_ERR_PRECONDITION;
    *copied = 0;
    directory = opendir(provider);
    if (!directory) return PW_ERR_NOT_FOUND;
    errno = 0;
    while ((entry = readdir(directory))) {
        size_t length = strlen(entry->d_name);
        if (length < 4 || strcasecmp(entry->d_name + length - 4, ".dll")) continue;
        if (length >= ZINK_DLL_NAME || count == ZINK_DLL_COUNT) { status = PW_ERR_LIMIT; break; }
        for (size_t i = 0; i < length; i++) {
            unsigned char c = (unsigned char)entry->d_name[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '+'))
                status = PW_ERR_MALFORMED;
        }
        for (unsigned i = 0; i < count; i++)
            if (!strcasecmp(names[i], entry->d_name)) status = PW_ERR_MALFORMED;
        if (status != PW_OK) break;
        strcpy(names[count++], entry->d_name);
        if (!strcasecmp(entry->d_name, "opengl32.dll")) core |= 1;
        if (!strcasecmp(entry->d_name, "libgallium_wgl.dll")) core |= 2;
    }
    if (!entry && errno) status = PW_ERR_STATE;
    closedir(directory);
    if (status != PW_OK) return status;
    if (core != 3) return PW_ERR_NOT_FOUND;
    /* Every companion is checked before any prefix file is replaced. */
    for (unsigned i = 0; i < count; i++) {
        FILE *file; size_t bytes;
        if (snprintf(source, sizeof(source), "%s/%s", provider, names[i]) >= (int)sizeof(source))
            return PW_ERR_LIMIT;
        if (!(file = fopen(source, "rb"))) return PW_ERR_NOT_FOUND;
        status = zink_dll_validate(file, architecture, &bytes);
        if (status == PW_OK) status = zink_imports(file, bytes, architecture, names, count);
        fclose(file);
        if (status != PW_OK) return status;
    }
    int base = snprintf(folder, sizeof(folder), "%s/drive_c/windows/", prefix);
    if (base < 0 || (size_t)base >= sizeof(folder) || snprintf(folder + base, sizeof(folder) - (size_t)base, "%s",
                            architecture == PW_APP_ARCH_PE32 ? "syswow64" : "system32") >=
                    (int)(sizeof(folder) - (size_t)base)) return PW_ERR_LIMIT;
    if (make_folders(folder, strlen(prefix))) return PW_ERR_STATE;
    for (unsigned i = 0; i < count; i++) {
        FILE *file; size_t bytes;
        if (snprintf(source, sizeof(source), "%s/%s", provider, names[i]) >= (int)sizeof(source) ||
            snprintf(destination, sizeof(destination), "%s/%s", folder, names[i]) >= (int)sizeof(destination))
            return PW_ERR_LIMIT;
        if (!(file = fopen(source, "rb"))) return PW_ERR_NOT_FOUND;
        status = zink_dll_validate(file, architecture, &bytes);
        if (status == PW_OK) {
            status = zink_copy(file, bytes, destination);
            if (status >= 0) { *copied += (unsigned)status; status = PW_OK; }
        }
        fclose(file);
        if (status != PW_OK) return status;
    }
    return PW_OK;
}
