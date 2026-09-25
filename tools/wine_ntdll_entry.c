/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Host runner for the bounded Wine ntdll entry gate.
 *
 * It loads a staged i386 Wine runtime, binds the real module graph, enters
 * one exported ntdll function through the IA-32 DBT and stops at the
 * versioned Unix-call boundary. The private input is a build artifact, never
 * a file in this repository.
 *
 *   wine_ntdll_entry --runtime .deps/wine-runtime
 *
 * Every line it prints is `kind=host-wine-...` telemetry consumed by
 * tools/validate_wine_ntdll_evidence.py.
 */
#include "../src/pw_file_posix.h"
#include "../src/pw_app_profile.h"
#include "../src/pw_vm_posix.h"
#include "../src/pw_wine_gate.h"
#include "../src/pw_wine_runner.h"
#include "../src/pw_wine_seed_services.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/utsname.h>

static PwWineGateReport report;
static PwWineRunner gate_runner;

/*
 * The host side of unix_wine_dbg_write for this runner. Wine's CU side does
 * `write(2, params->str, params->len)`; the transcript is public evidence, so
 * this sink deliberately does *not* keep or print the guest's text - it counts
 * the writes and the bytes, and the report records the same numbers. A private
 * capture can be added later behind an explicit flag.
 */
typedef struct RunnerDebugSink {
    uint64_t writes;
    uint64_t bytes;
} RunnerDebugSink;

static RunnerDebugSink debug_sink_state;

static int runner_debug_write(void *context, const void *bytes,
                              uint32_t length)
{
    RunnerDebugSink *sink = context;

    (void)bytes;
    sink->writes++;
    sink->bytes += length;
    return 0;
}

static const PwWineDebugSink debug_sink = {
    .context = &debug_sink_state, .write = runner_debug_write,
};

static int load_host_app_profile(const char *path, PwAppProfile *profile)
{
    uint8_t bytes[PW_APP_PROFILE_MAX_BYTES];
    FILE *file;
    size_t length;
    int too_large;
    int read_error;
    int close_error;

    if (!path || !profile)
        return PW_ERR_PRECONDITION;
    file = fopen(path, "rb");
    if (!file)
        return PW_ERR_NOT_FOUND;
    length = fread(bytes, 1u, sizeof(bytes), file);
    read_error = ferror(file);
    too_large = fgetc(file) != EOF;
    close_error = fclose(file) != 0;
    if (read_error || too_large || close_error)
        return too_large ? PW_ERR_LIMIT : PW_ERR_STATE;
    return pw_app_profile_parse(bytes, length, profile);
}

static int profile_application_directory(const PwAppProfile *profile,
                                         char *output, size_t capacity)
{
    const char *separator;
    size_t length;

    if (!profile || !output || capacity == 0u)
        return PW_ERR_PRECONDITION;
    separator = strrchr(profile->executable, '\\');
    if (!separator)
        return PW_ERR_MALFORMED;
    length = (size_t)(separator - profile->executable) + 1u;
    if (length == 0u || length >= capacity)
        return PW_ERR_LIMIT;
    memcpy(output, profile->executable, length);
    output[length] = '\0';
    return PW_OK;
}

/*
 * Host file service below the Unix-call boundary. The gate has already
 * translated the guest path into a canonical name inside the runtime
 * distribution; this only opens, reads and closes it, and it never sees a
 * guest pointer.
 */
/*
 * Where the file service looks: one directory per namespace, because the same
 * name can exist in both and the gate has already said which root the guest's
 * path named. Nothing falls back from one to the other.
 */
static char host_application_directory[512];
static char host_runtime_directory[512];
/* The distribution's NLS data (its nls/ directory). Wine looks there first and
 * falls back to the Windows system directory (dlls/ntdll/unix/env.c:120
 * read_nls_file and open_nls_data_file), so a name in the runtime namespace is
 * tried in that order here too. */
static char host_nls_directory[512];

static PwWineFileStatus host_file_open(void *context, PwFileNamespace file_namespace,
                                       const char *name, uint64_t *size,
                                       void **token)
{
    char path[1024];
    const char *directory;
    FILE *file;
    long length;

    (void)context;
    file = NULL;
    if (file_namespace == PW_FILE_RUNTIME && host_nls_directory[0] != '\0' &&
        snprintf(path, sizeof(path), "%s/%s", host_nls_directory, name) <
            (int)sizeof(path))
        file = fopen(path, "rb");
    if (!file) {
        directory = file_namespace == PW_FILE_APPLICATION
            ? host_application_directory : host_runtime_directory;
        if (directory[0] == '\0')
            return PW_WINE_FILE_NOT_FOUND;
        if (snprintf(path, sizeof(path), "%s/%s", directory, name) >=
            (int)sizeof(path))
            return PW_WINE_FILE_ERROR;
        file = fopen(path, "rb");
    }
    if (!file)
        return errno == EACCES ? PW_WINE_FILE_DENIED : PW_WINE_FILE_NOT_FOUND;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return PW_WINE_FILE_ERROR;
    }
    length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return PW_WINE_FILE_ERROR;
    }
    *size = (uint64_t)length;
    *token = file;
    return PW_WINE_FILE_OK;
}

static PwWineFileStatus host_file_read(void *context, void *token,
                                       uint64_t offset, void *bytes,
                                       uint32_t size, uint32_t *read_bytes)
{
    FILE *file = token;

    (void)context;
    if (!file || fseek(file, (long)offset, SEEK_SET) != 0)
        return PW_WINE_FILE_ERROR;
    *read_bytes = (uint32_t)fread(bytes, 1u, size, file);
    return PW_WINE_FILE_OK;
}

static void host_file_close(void *context, void *token)
{
    (void)context;
    if (token)
        fclose(token);
}

static const PwWineFileService host_files = {
    .context = NULL, .open = host_file_open, .read = host_file_read,
    .close = host_file_close,
};

/*
 * Host registry service: the registry content this distribution declares.
 *
 * It is the key ntdll opens unconditionally during startup, with the values
 * Wine's own initial registry (`loader/wine.inf` at the pinned revision)
 * gives it - nothing here is invented. Everything else, and every value the
 * profile does not declare, answers STATUS_OBJECT_NAME_NOT_FOUND so ntdll
 * falls back to its compiled-in defaults; that is what the two search-mode
 * options the loader asks for are, and why they are deliberately absent.
 */
#define HOST_REG_DWORD 4u   /* REG_DWORD */

typedef struct HostRegistryValue {
    const char *name;       /* canonical lower-case value name */
    uint32_t type;
    uint32_t dword;
} HostRegistryValue;

typedef struct HostRegistryKey {
    const char *path;       /* canonical lower-case NT key path */
    const HostRegistryValue *values;
    uint32_t value_count;
} HostRegistryKey;

static const HostRegistryValue host_session_manager_values[] = {
    { "criticalsectiontimeout", HOST_REG_DWORD, 0x00278d00u },
    { "globalflag", HOST_REG_DWORD, 0u },
    { "heapdecommitfreeblockthreshold", HOST_REG_DWORD, 0u },
    { "heapdecommittotalfreethreshold", HOST_REG_DWORD, 0u },
    { "heapsegmentcommit", HOST_REG_DWORD, 0u },
    { "heapsegmentreserve", HOST_REG_DWORD, 0u },
};

/*
 * The profile is a *store*, not a fixed list: a process's own startup creates
 * keys (its user hive root, its HKCU software tree) and opens them again, and a
 * runtime that answers "no such key" to a key it was just asked to create
 * leaves its caller retrying. The table below is what the distribution
 * declares when it starts, plus everything the guest has created since; it
 * lives for the length of the run, which is the honest scope until a
 * file-backed hive exists (measured: the pinned runtime's locale startup asks
 * for HKCU\Control Panel hundreds of times in a row and never gets it).
 */
#define HOST_REGISTRY_MAX_KEYS 128

/* A copy of a path the guest created, owned by the store for the run. */
static char *host_strdup(const char *text)
{
    const size_t bytes = strlen(text) + 1u;
    char *copy = malloc(bytes);

    if (copy)
        memcpy(copy, text, bytes);
    return copy;
}

static HostRegistryKey host_registry_keys[HOST_REGISTRY_MAX_KEYS] = {
    {
        "\\registry\\machine\\system\\currentcontrolset\\control\\session "
        "manager",
        host_session_manager_values,
        sizeof(host_session_manager_values) /
            sizeof(host_session_manager_values[0]),
    },
    /*
     * The user hive root RtlOpenCurrentUser opens (with NtCreateKey) after it
     * has formatted the token's SID into the path.
     */
    { "\\registry\\user\\s-1-5-21-0-0-0-1000", NULL, 0u },
};

static unsigned host_registry_key_count = 2u;   /* the two declared keys */

static PwWineRegistryStatus host_registry_open(void *context, const char *path,
                                               void **token)
{
    (void)context;
    for (unsigned index = 0; index < host_registry_key_count; ++index) {
        if (strcmp(host_registry_keys[index].path, path) != 0)
            continue;
        *token = (void *)(uintptr_t)&host_registry_keys[index];
        return PW_WINE_REGISTRY_OK;
    }
    return PW_WINE_REGISTRY_NOT_FOUND;
}

/*
 * The values the guest has written, which a later query on the same key reads
 * back. NT replaces the value of that name, so a second write to the same name
 * overwrites the first; the pool is bounded and a store that is full answers
 * the failure of the write rather than dropping it.
 */
#define HOST_REGISTRY_MAX_STORED_VALUES 64
#define HOST_REGISTRY_MAX_VALUE_BYTES PW_WINE_GATE_MAX_VALUE

typedef struct HostStoredValue {
    HostRegistryKey *key;
    char name[128];
    uint32_t type;
    uint32_t size;
    uint32_t used;
    uint8_t bytes[HOST_REGISTRY_MAX_VALUE_BYTES];
} HostStoredValue;

static HostStoredValue host_stored_values[HOST_REGISTRY_MAX_STORED_VALUES];

static PwWineRegistryStatus host_registry_query(void *context, void *token,
                                                const char *value,
                                                uint32_t *type,
                                                const void **bytes,
                                                uint32_t *size)
{
    const HostRegistryKey *key = token;

    (void)context;
    if (!key)
        return PW_WINE_REGISTRY_ERROR;
    for (uint32_t index = 0; index < key->value_count; ++index) {
        if (strcmp(key->values[index].name, value) != 0)
            continue;
        *type = key->values[index].type;
        *bytes = &key->values[index].dword;
        *size = sizeof(uint32_t);
        return PW_WINE_REGISTRY_OK;
    }
    for (uint32_t index = 0; index < HOST_REGISTRY_MAX_STORED_VALUES;
         ++index) {
        if (!host_stored_values[index].used ||
            host_stored_values[index].key != key ||
            strcmp(host_stored_values[index].name, value) != 0)
            continue;
        *type = host_stored_values[index].type;
        *bytes = host_stored_values[index].bytes;
        *size = host_stored_values[index].size;
        return PW_WINE_REGISTRY_OK;
    }
    return PW_WINE_REGISTRY_NOT_FOUND;
}

/*
 * NtSetValueKey: store the value the gate read out of the guest's own buffer.
 * A name the key already carries is replaced, which is what NT does; a store
 * with no room left for a new name answers ERROR so the guest is told the write
 * did not happen.
 */
static PwWineRegistryStatus host_registry_set_value(void *context, void *token,
                                                    const char *value,
                                                    uint32_t type,
                                                    const void *bytes,
                                                    uint32_t size)
{
    HostRegistryKey *key = token;
    HostStoredValue *free_slot = NULL;

    (void)context;
    if (!key || size > HOST_REGISTRY_MAX_VALUE_BYTES ||
        strlen(value) >= sizeof(host_stored_values[0].name))
        return PW_WINE_REGISTRY_ERROR;
    for (uint32_t index = 0; index < HOST_REGISTRY_MAX_STORED_VALUES;
         ++index) {
        HostStoredValue *stored = &host_stored_values[index];

        if (!stored->used) {
            if (!free_slot)
                free_slot = stored;
            continue;
        }
        if (stored->key == key && strcmp(stored->name, value) == 0) {
            stored->type = type;
            stored->size = size;
            if (size)
                memcpy(stored->bytes, bytes, size);
            return PW_WINE_REGISTRY_OK;
        }
    }
    if (!free_slot)
        return PW_WINE_REGISTRY_ERROR;
    free_slot->key = key;
    memcpy(free_slot->name, value, strlen(value) + 1u);
    free_slot->type = type;
    free_slot->size = size;
    if (size)
        memcpy(free_slot->bytes, bytes, size);
    free_slot->used = 1u;
    return PW_WINE_REGISTRY_OK;
}

/*
 * NtCreateKey is create-or-open, so a path the store already holds answers
 * REG_OPENED_EXISTING_KEY and one it does not is added and answers
 * REG_CREATED_NEW_KEY. The addition is bounded: a run that creates more keys
 * than the store holds is answered NOT_FOUND, which is what a full hive would
 * look like from the guest's side and is counted as a refusal by the gate.
 */
static PwWineRegistryStatus host_registry_create(void *context,
                                                 const char *path,
                                                 void **token,
                                                 uint32_t *created)
{
    const PwWineRegistryStatus status =
        host_registry_open(context, path, token);

    if (status == PW_WINE_REGISTRY_OK) {
        *created = 0u;
        return status;
    }
    if (status != PW_WINE_REGISTRY_NOT_FOUND ||
        host_registry_key_count >= HOST_REGISTRY_MAX_KEYS ||
        strlen(path) > 2u * PW_WINE_GATE_MAX_PATH)
        return status;
    host_registry_keys[host_registry_key_count].path = host_strdup(path);
    if (!host_registry_keys[host_registry_key_count].path)
        return PW_WINE_REGISTRY_ERROR;
    host_registry_keys[host_registry_key_count].values = NULL;
    host_registry_keys[host_registry_key_count].value_count = 0u;
    *token = (void *)(uintptr_t)&host_registry_keys[host_registry_key_count];
    host_registry_key_count++;
    *created = 1u;
    return PW_WINE_REGISTRY_OK;
}

static void host_registry_close(void *context, void *token)
{
    (void)context;
    (void)token;
}

static const PwWineRegistryService host_registry = {
    .context = NULL, .open = host_registry_open, .query = host_registry_query,
    .create = host_registry_create, .set_value = host_registry_set_value,
    .close = host_registry_close,
};

/*
 * The token the process runs as, as the host declares it: the local user SID
 * Wine's own server uses (server/token.c, local_user_sid, at the pinned
 * revision), which is S-1-5-21-0-0-0-1000. ntdll formats it into
 * \Registry\User\<SID> before it opens HKCU, and RtlConvertSidToUnicodeString
 * in the guest renders it from these bytes, so the identity the guest reports
 * is the one this profile declares rather than a compiled-in guess.
 */
static const uint8_t host_user_sid[] = {
    0x01, 0x05,                             /* revision, subauthority count */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x05,     /* SECURITY_NT_AUTHORITY */
    0x15, 0x00, 0x00, 0x00,                 /* SECURITY_NT_NON_UNIQUE, 21 */
    0x00, 0x00, 0x00, 0x00,                 /* 0 */
    0x00, 0x00, 0x00, 0x00,                 /* 0 */
    0x00, 0x00, 0x00, 0x00,                 /* 0 */
    0xe8, 0x03, 0x00, 0x00,                 /* 1000 */
};

/*
 * The object namespace this distribution declares: the \KnownDlls directory
 * the loader opens once at startup, and no section objects in it. That is the
 * honest profile for a distribution that carries PE files rather than a
 * preloaded section namespace - Wine then falls back to loading a module from
 * the file system, which is what it does on a prefix without known DLLs.
 */
static const char *host_object_directories[] = {
    "\\knowndlls",
};

static PwWineObjectStatus host_object_open(void *context,
                                           PwWineObjectKind kind,
                                           const char *path, void **token)
{
    (void)context;
    if (kind == PW_WINE_OBJECT_DIRECTORY) {
        for (unsigned index = 0;
             index < sizeof(host_object_directories) /
                         sizeof(host_object_directories[0]);
             ++index) {
            if (strcmp(host_object_directories[index], path) != 0)
                continue;
            *token = (void *)(uintptr_t)host_object_directories[index];
            return PW_WINE_OBJECT_OK;
        }
    }
    /* No section is preloaded: \KnownDlls\<name> is not in this namespace. */
    return PW_WINE_OBJECT_NOT_FOUND;
}

static void host_object_close(void *context, void *token)
{
    (void)context;
    (void)token;
}

static const PwWineObjectService host_objects = {
    .context = NULL, .open = host_object_open, .close = host_object_close,
};

/*
 * The version block ntdll asks for with SystemWineVersionInformation: the
 * four NUL-terminated strings Wine packs together (version, build id, host
 * system name, host release). It is built from the staged distribution's own
 * manifest, never from a compiled-in constant, so wine_get_version in the
 * guest reports the modules actually being executed. Wine's build id is
 * "wine-<version>" when its checkout has no describable tag, which is the
 * case for the pinned revision this distribution is built from.
 */
static char wine_version_info[4u * 64u];
static uint32_t wine_version_info_bytes;

/* Reads one quoted string field out of the manifest without a JSON library:
 * the file is generated by tools/validate_wine_runtime.py, so the field is
 * known to be a plain quoted string and anything else fails closed. */
static int manifest_field(const char *path, const char *field, char *out,
                          size_t out_bytes)
{
    FILE *file = fopen(path, "rb");
    char text[64u * 1024u];
    char pattern[64];
    size_t used;
    const char *found;
    const char *cursor;
    size_t length = 0u;

    if (!file)
        return 0;
    used = fread(text, 1u, sizeof(text) - 1u, file);
    fclose(file);
    text[used] = '\0';
    if (snprintf(pattern, sizeof(pattern), "\"%s\"", field) >=
        (int)sizeof(pattern))
        return 0;
    found = strstr(text, pattern);
    if (!found)
        return 0;
    cursor = strchr(found + strlen(pattern), ':');
    if (!cursor)
        return 0;
    cursor++;
    while (*cursor == ' ' || *cursor == '\t')
        ++cursor;
    if (*cursor != '"')
        return 0;
    ++cursor;
    while (*cursor != '\0' && *cursor != '"' && *cursor != '\n' &&
           length + 1u < out_bytes)
        out[length++] = *cursor++;
    if (*cursor != '"' || length == 0u)
        return 0;
    out[length] = '\0';
    return 1;
}

static int build_wine_version_info(const char *manifest)
{
    char version[64];
    const char *host_system = "Linux";
    const char *host_release = "6";
    const char *fields[4];
    size_t used = 0u;

    if (!manifest_field(manifest, "version", version, sizeof(version))) {
        fprintf(stderr, "cannot read the Wine version from %s\n", manifest);
        return 0;
    }
    {
        /* The manifest records Wine's own version string, "Wine version X.Y";
         * the guest's first field is that version, without the prefix. */
        static const char prefix[] = "Wine version ";

        if (strncmp(version, prefix, sizeof(prefix) - 1u) == 0)
            memmove(version, version + sizeof(prefix) - 1u,
                    strlen(version + sizeof(prefix) - 1u) + 1u);
    }
    fields[0] = version;
    {
        static char build[80];

        snprintf(build, sizeof(build), "wine-%s", version);
        fields[1] = build;
    }
    {
        /* Wine reports the host the process runs on, so this is the only
         * field that is a property of the host rather than of the manifest. */
        static char system_name[128];
        static char release[128];
        struct utsname buf;

        if (uname(&buf) == 0) {
            snprintf(system_name, sizeof(system_name), "%s", buf.sysname);
            snprintf(release, sizeof(release), "%s", buf.release);
            host_system = system_name;
            host_release = release;
        }
    }
    fields[2] = host_system;
    fields[3] = host_release;
    for (unsigned index = 0; index < 4u; ++index) {
        const size_t length = strlen(fields[index]) + 1u;

        if (used + length > sizeof(wine_version_info))
            return 0;
        memcpy(wine_version_info + used, fields[index], length);
        used += length;
    }
    wine_version_info_bytes = (uint32_t)used;
    return 1;
}



static void trace_step(void *context, const PwX86State *state)
{
    (void)context;
    printf("kind=host-wine-step eip=0x%08x esp=0x%08x eax=0x%08x "
           "ebx=0x%08x ecx=0x%08x edx=0x%08x esi=0x%08x edi=0x%08x "
           "ebp=0x%08x fs=0x%08x\n", state->eip, state->gpr[4], state->gpr[0],
           state->gpr[3], state->gpr[1], state->gpr[2], state->gpr[6],
           state->gpr[7], state->gpr[5], state->fs_base);
    /* Flush per step: a mode that faults must still leave its trace. */
    fflush(stdout);
}

/*
 * Every transcript line is a space-separated field list, and registry key
 * names do contain spaces ("Session Manager", "Windows NT"), so a recorded
 * path is printed with each space escaped as %20. Nothing else is escaped:
 * a canonical path is printable ASCII by construction.
 */
static const char *recorded_path(const char *path, char *out, size_t out_bytes)
{
    size_t used = 0u;

    for (size_t index = 0; path[index] != '\0'; ++index) {
        static const char escape[] = "%20";

        if (path[index] == ' ') {
            if (used + sizeof(escape) - 1u >= out_bytes)
                break;
            memcpy(out + used, escape, sizeof(escape) - 1u);
            used += sizeof(escape) - 1u;
        } else {
            if (used + 1u >= out_bytes)
                break;
            out[used++] = path[index];
        }
    }
    out[used] = '\0';
    return out;
}

static const char *argument_value(int argc, char **argv, const char *name,
                                  const char *fallback)
{
    for (int index = 1; index + 1 < argc; ++index)
        if (strcmp(argv[index], name) == 0)
            return argv[index + 1];
    return fallback;
}

static unsigned argument_number(int argc, char **argv, const char *name,
                                unsigned fallback)
{
    const char *text = argument_value(argc, argv, name, NULL);

    if (!text)
        return fallback;
    return (unsigned)strtoul(text, NULL, 0);
}

static void print_module(const PwWineModuleRecord *module)
{
    printf("kind=host-wine-module name=%s sha256=%s size=%u machine=0x%04x "
           "base=0x%08x image_bytes=%u loaded=%u runtime=%u imports=%u tls=%u "
           "path=%s\n",
           module->name, module->sha256, module->size, module->machine,
           module->base, module->image_bytes, module->loaded, module->runtime,
           module->imports, module->tls_present, module->path);
}

int main(int argc, char **argv)
{
    PwWineGateConfig config;
    PwWineSeedServices seed_services;
    PwFilePosix files;
    PwFileProvider provider;
    PwVmBackend vm;
    /* The provider opens a canonical module name inside one directory, so
     * --runtime is the staged PE library, not the distribution root. */
    const char *runtime = argument_value(argc, argv, "--runtime",
                                         ".deps/wine-runtime/lib/i386-windows");
    const char *entry_module = argument_value(argc, argv, "--entry-module",
                                              "ntdll.dll");
    const char *entry_symbol = argument_value(argc, argv, "--entry-symbol",
                                              "NtClose");
    const char *root = argument_value(argc, argv, "--root", "kernelbase.dll");
    const char *profile_path = argument_value(argc, argv, "--profile", NULL);
    /* The application namespace: the directory a Windows loader searches first.
     * Defaults to the runtime directory, which is what the ntdll control wants;
     * an application fixture lives elsewhere and is reached through
     * PW_FILE_APPLICATION only. */
    const char *application = argument_value(argc, argv, "--application",
                                             runtime);
    /* The distribution's NLS data: the "installed" nls/ directory of the
     * staged runtime, or empty to leave those names unanswered. */
    const char *nls = argument_value(argc, argv, "--nls", "");
    const char *dlls = argument_value(argc, argv, "--modules",
                                      "ntdll.dll,kernelbase.dll");
    PwAppProfile app_profile;
    char profile_root[PW_APP_PATH_CAPACITY];
    char profile_app_dir[PW_APP_PATH_CAPACITY];
    char profile_command_line[PW_APP_ARGUMENTS_CAPACITY];
    int have_profile = 0;
    const int use_seed_services =
        argument_number(argc, argv, "--seed-services", 0u) != 0u;
    int status;

    if (!runtime) {
        fprintf(stderr, "usage: wine_ntdll_entry [--runtime lib/i386-windows] "
                        "[--entry-module ntdll.dll] [--entry-symbol NtClose] "
                        "[--root kernelbase.dll] [--application DIR] "
                        "[--root-application 0|1] [--budget N]\n");
        return 2;
    }
    if (pw_file_posix_init(&files, application) != PW_OK ||
        pw_file_posix_set_runtime_directory(&files, runtime) != PW_OK ||
        pw_file_posix_provider(&files, &provider) != PW_OK ||
        pw_vm_posix_backend(&vm) != PW_OK) {
        fprintf(stderr, "cannot initialise the runtime provider\n");
        return 2;
    }
    memset(&config, 0, sizeof(config));
    if (profile_path) {
        status = load_host_app_profile(profile_path, &app_profile);
        if (status == PW_OK && app_profile.architecture != PW_APP_ARCH_PE32)
            status = PW_ERR_UNSUPPORTED;
        if (status == PW_OK)
            status = pw_app_profile_stage_name(&app_profile, profile_root,
                                               sizeof(profile_root));
        if (status == PW_OK)
            status = profile_application_directory(&app_profile,
                profile_app_dir, sizeof(profile_app_dir));
        if (status == PW_OK)
            status = pw_app_profile_build_command_line(&app_profile,
                profile_command_line, sizeof(profile_command_line));
        if (status != PW_OK) {
            fprintf(stderr, "invalid app profile %s: %s\n", profile_path,
                    pw_result_name(status));
            return 2;
        }
        root = profile_root;
        have_profile = 1;
    }
    if (use_seed_services)
        pw_wine_seed_services_init(&seed_services);
    pw_wine_runner_init(&gate_runner);
    config.runner = &gate_runner;
    config.root_application = (uint8_t)(have_profile ||
        argument_number(argc, argv, "--root-application", 0u) != 0u);
    config.provider = &provider;
    config.backend = &vm;
    {
        static PwWineFileService files = host_files;

        (void)snprintf(host_runtime_directory, sizeof(host_runtime_directory),
                       "%s", runtime);
        (void)snprintf(host_application_directory,
                       sizeof(host_application_directory), "%s", application);
        /*
         * The NLS data of the staged distribution lives beside the modules it
         * serves (nls/ next to lib/i386-windows/), which is where Wine keeps
         * it; --nls overrides that for a distribution staged differently.
         */
        if (nls && nls[0] != '\0')
            (void)snprintf(host_nls_directory, sizeof(host_nls_directory),
                           "%s", nls);
        else
            (void)snprintf(host_nls_directory, sizeof(host_nls_directory),
                           "%s/../../nls", runtime);
        config.files = &files;
    }
    config.root_module = root;
    config.entry_module = entry_module;
    config.entry_symbol = entry_symbol;
    if (have_profile) {
        config.process_image_path = app_profile.executable;
        config.process_current_directory = app_profile.working_directory;
        config.process_application_directory =
            profile_app_dir;
        config.process_command_line = profile_command_line;
    }
    if (use_seed_services) {
        config.registry = pw_wine_seed_registry(&seed_services);
        config.objects = pw_wine_seed_objects(&seed_services);
        config.token_user_sid = pw_wine_seed_user_sid(
            &config.token_user_sid_bytes);
    } else {
        config.registry = &host_registry;
        config.objects = &host_objects;
        config.token_user_sid = host_user_sid;
        config.token_user_sid_bytes = (uint32_t)sizeof(host_user_sid);
    }
    {
        const char *manifest = argument_value(
            argc, argv, "--manifest",
            ".deps/wine-runtime/wine-runtime-manifest.json");

        if (build_wine_version_info(manifest)) {
            config.wine_version_info = wine_version_info;
            config.wine_version_info_bytes = wine_version_info_bytes;
        } else {
            /* Fail closed: the gate answers STATUS_NOT_SUPPORTED for the Wine
             * version rather than reporting one that is not the distribution
             * being run. */
            fprintf(stderr, "warning: no Wine version from %s; the version "
                            "information class will answer NOT_SUPPORTED\n",
                    manifest);
        }
    }
    config.step_budget = argument_number(argc, argv, "--budget", 0u);
    config.trace = argument_value(argc, argv, "--trace", NULL) ? trace_step
                                                               : NULL;
    config.bridge_calls = (uint8_t)(argument_number(argc, argv, "--bridge", 0u) != 0u);
    config.unixlib_calls = (uint8_t)(argument_number(argc, argv, "--unixlib", 0u) != 0u);
    config.debug_sink = config.unixlib_calls ? &debug_sink : NULL;
    config.call_budget = argument_number(argc, argv, "--call-budget", 0u);
    {
        const char *modes = argument_value(argc, argv, "--modes", NULL);

        if (modes) {
            /* chaining,residency,lazy-flags: 0 or 1 each. */
            unsigned values[3] = {1u, 1u, 1u};

            for (unsigned index = 0; index < 3u && *modes; ++index) {
                values[index] = (unsigned)strtoul(modes, NULL, 0) != 0u;
                const char *comma = strchr(modes, ',');
                modes = comma ? comma + 1 : modes + strlen(modes);
            }
            config.chaining = (uint8_t)values[0];
            config.residency = (uint8_t)values[1];
            config.lazy_flags = (uint8_t)values[2];
            config.modes_set = 1u;
        }
    }
    {
        char *copy = malloc(strlen(dlls) + 1u);

        if (!copy)
            return 2;
        memcpy(copy, dlls, strlen(dlls) + 1u);
        for (char *name = strtok(copy, ","); name != NULL;
             name = strtok(NULL, ",")) {
            if (config.module_count >= PW_WINE_GATE_MAX_MODULES)
                break;
            config.modules[config.module_count++] = name;
        }
        printf("kind=host-wine-gate runtime=%s root=%s entry=%s!%s "
               "modules=%u\n", runtime, root, entry_module, entry_symbol,
               config.module_count);
        status = pw_wine_gate_run(&config, &report);
        free(copy);
    }
    for (uint32_t index = 0; index < report.module_count; ++index)
        print_module(&report.modules[index]);
    printf("kind=host-wine-bind modules=%u functions=%u data=%u failures=%u\n",
           report.bound_modules, report.bound_functions, report.bound_data,
           report.bind_failures);
    printf("kind=host-wine-tls modules=%u\n", report.tls_modules);
    printf("kind=host-wine-boundary slot_rva=0x%08x slot_va=0x%08x "
           "thunks=%u thunk_rva=0x%08x thunk_va=0x%08x\n",
           report.boundary_slot_rva, report.boundary_slot_va,
           report.boundary_count, report.boundary_thunk_rva,
           report.boundary_thunk_va);
    printf("kind=host-wine-entry module=%s symbol=%s rva=0x%08x eip=0x%08x "
           "pe_entry_rva=0x%08x stub_id=0x%08x kind=%s "
           "thread_start_rva=0x%08x thread_start_eip=0x%08x "
           "main_entry_eip=0x%08x\n",
           entry_module, entry_symbol, report.entry_rva, report.entry_eip,
           report.entry_pe_rva, report.stub_syscall_id,
           report.stub_syscall_id != 0u ? "stub" : "initialization",
           report.thread_start_rva, report.thread_start_eip,
           report.main_entry_eip);
    printf("kind=host-wine-call return_eip=0x%08x in_module=%u caller_rva=0x%08x "
           "caller_id=0x%08x observed=0x%08x\n",
           report.boundary_return_eip, report.boundary_return_in_module,
           report.caller_stub_rva, report.caller_stub_id,
           report.observed_syscall_id);
    printf("kind=host-wine-modes chaining=%u residency=%u lazy_flags=%u\n",
           report.chaining, report.residency, report.lazy_flags);
    printf("kind=host-wine-run first_eip=0x%08x last_eip=0x%08x "
           "retired=%llu dispatches=%llu blocks=%llu bytes=%llu "
           "reg_loads=%llu reg_stores=%llu reg_reconciliations=%llu "
           "reg_spills=%llu "
           "stop_address=0x%08x stop=%s syscall=0x%08x host_calls=%llu\n",
           report.first_eip, report.last_eip,
           (unsigned long long)report.retired,
           (unsigned long long)report.dispatches,
           (unsigned long long)report.translated_blocks,
           (unsigned long long)report.translated_bytes,
           (unsigned long long)report.register_loads,
           (unsigned long long)report.register_stores,
           (unsigned long long)report.register_reconciliations,
           (unsigned long long)report.register_spills,
           report.stop_address, pw_wine_stop_name(report.stop),
           report.observed_syscall_id, (unsigned long long)report.host_calls);
    if (report.stop == PW_WINE_STOP_MEMORY_BOUNDS) {
        uint32_t index;

        printf("kind=host-wine-fault address=0x%08x width=%u write=%u "
               "block=0x%08x block_instructions=%u resident=%#x map=",
               report.fault_address, report.fault_width, report.fault_write,
               report.fault_block_pc, report.fault_block_instructions,
               report.fault_block_resident_mask);
        for (index = 0u; index < 8u; ++index)
            printf("%d,", report.fault_block_map[index]);
        printf(" code=");
        for (index = 0u; index < report.fault_block_code_bytes; ++index)
            printf("%02x", report.fault_block_code[index]);
        printf(" recent=");
        for (index = 0u; index < report.recent_count; ++index)
            printf("%s0x%08x", index ? "," : "", report.recent_pcs[index]);
        printf("\n");
    }
    /*
     * The cleanup verdict is about releasing what this run mapped and
     * translated; the run's own result is already carried by the stop kind,
     * because a bridged run that meets an unimplemented instruction family
     * stops honestly without being a leak.
     *
     * The verdict comes from what the releases did, not from what the run
     * mapped: `failures` counts teardown actions that failed and the pending
     * fields identify every kind of owner left after the attempt. Unit-level
     * owners can be retried; this one-shot gate propagates a persistent failure
     * through gate_status. The validator rejects every incomplete transcript.
     */
    printf("kind=host-wine-cleanup modules=%u pending_modules=%u "
           "pending_pages=%u pending_regions=%u mappings=%u translations=%u "
           "pending_translations=%u failures=%u "
           "status=%s gate_status=%s\n", report.cleanup_modules,
           report.cleanup_modules_pending,
           report.cleanup_process_pages_pending,
           report.cleanup_call_regions_pending,
           report.cleanup_mappings, report.cleanup_translations,
           report.cleanup_translations_pending,
           report.cleanup_failures,
           (report.cleanup_modules == report.module_count &&
            report.cleanup_modules_pending == 0u &&
            report.cleanup_process_pages_pending == 0u &&
            report.cleanup_call_regions_pending == 0u &&
            report.cleanup_translations_pending == 0u &&
            report.cleanup_failures == 0u &&
            report.cleanup_translations >= 1u &&
            report.cleanup_mappings >= report.module_count) ? "ok"
                                                           : "incomplete",
           pw_result_name(report.status));
    /* A low reservation that ran out of candidates falls back to an address a
     * PE32 image cannot use; the evidence says so instead of leaving the
     * rejection unexplained. */
    printf("kind=host-wine-low exhausted=%u stage=%s\n", report.low_exhausted,
           report.gate_stage ? report.gate_stage : "unknown");
    if (config.unixlib_calls) {
        printf("kind=host-wine-unixlib boundary=0x%08x slot=0x%08x "
               "handle=0x%08x handle_slot=0x%08x serviced=%llu "
               "unknown_handle=%llu unknown_code=%llu malformed=%llu "
               "service_failed=%llu unimplemented=%llu unsupported=%llu "
               "debug_bytes=%llu "
               "sink_writes=%llu last_code=%u last_status=%s "
               "last_return_pc=0x%08x last_args=0x%08x\n",
               report.unixlib_boundary_va, report.unixlib_dispatcher_slot_va,
               report.unixlib_handle, report.unixlib_handle_slot_va,
               (unsigned long long)report.unixlib.serviced,
               (unsigned long long)report.unixlib.unknown_handle,
               (unsigned long long)report.unixlib.unknown_code,
               (unsigned long long)report.unixlib.malformed,
               (unsigned long long)report.unixlib.service_failed,
               (unsigned long long)report.unixlib.unimplemented,
               (unsigned long long)report.unixlib.unsupported,
               (unsigned long long)report.unixlib.debug_bytes,
               (unsigned long long)debug_sink_state.writes,
               report.unixlib.last_code,
               pw_wine_unixlib_status_name(
                   (PwWineUnixlibStatus)report.unixlib.last_status),
               report.unixlib.last_return_pc, report.unixlib.last_args);
    }
    if (config.bridge_calls) {
        char file_path[2u * (PW_WINE_GATE_MAX_PATH + 1u)];
        char key_path[2u * (PW_WINE_GATE_MAX_PATH + 1u)];
        char object_path[2u * (PW_WINE_GATE_MAX_PATH + 1u)];

        printf("kind=host-wine-calls serviced=%u handled=%llu unimplemented=%llu "
               "unknown=%llu rejected=%llu allocations=%u releases=%u "
               "allocated_bytes=%u "
               "regions=%u files=%u opens=%llu reads=%llu bytes=%llu closes=%llu "
               "directories=%llu refusals=%llu fs_controls=%llu "
               "sections=%llu section_queries=%llu views=%llu protects=%llu last=%s "
               "registry=%u key_opens=%llu key_queries=%llu key_values=%llu "
               "key_creates=%llu key_refusals=%llu tokens=%llu last_key=%s "
               "objects=%u object_opens=%llu object_refusals=%llu "
               "last_object=%s processes=%llu image_characteristics=0x%04x "
               "vm_queries=%llu address_compares=%llu "
               "address_compare_refusals=%llu nls_maps=%llu "
               "nls_refusals=%llu\n",
               report.calls_serviced,
               (unsigned long long)report.calls.handled,
               (unsigned long long)report.calls.unimplemented,
               (unsigned long long)report.calls.unknown,
               (unsigned long long)report.calls.rejected, report.allocations,
               report.releases, report.allocated_bytes, report.call_regions,
               report.files_configured, (unsigned long long)report.file_opens,
               (unsigned long long)report.file_reads,
               (unsigned long long)report.file_bytes,
               (unsigned long long)report.file_closes,
               (unsigned long long)report.file_directories,
               (unsigned long long)report.file_refusals,
               (unsigned long long)report.file_fs_controls,
               (unsigned long long)report.section_creates,
               (unsigned long long)report.section_queries,
               (unsigned long long)report.section_views,
               (unsigned long long)report.section_protects,
               recorded_path(report.last_file[0] ? report.last_file : "-",
                             file_path, sizeof(file_path)),
               report.registry_configured,
               (unsigned long long)report.key_opens,
               (unsigned long long)report.key_queries,
               (unsigned long long)report.key_values,
               (unsigned long long)report.key_creates,
               (unsigned long long)report.key_refusals,
               (unsigned long long)report.token_queries,
               recorded_path(report.last_key[0] ? report.last_key : "-",
                             key_path, sizeof(key_path)),
               report.objects_configured,
               (unsigned long long)report.object_opens,
               (unsigned long long)report.object_refusals,
               recorded_path(report.last_object[0] ? report.last_object : "-",
                             object_path, sizeof(object_path)),
               (unsigned long long)report.process_queries,
               report.process_image_characteristics,
               (unsigned long long)report.virtual_queries,
               (unsigned long long)report.address_comparisons,
               (unsigned long long)report.address_comparison_refusals,
               (unsigned long long)report.nls_maps,
               (unsigned long long)report.nls_refusals);
        for (uint32_t index = 0; index < report.calls.records; ++index) {
            const PwUnixCallRecord *record = &report.calls.sequence[index];

            printf("kind=host-wine-call-seq index=%u id=0x%08x name=%s "
                   "args=%u stub_return=0x%08x return=0x%08x status=0x%08x "
                   "outcome=%s argument=%u\n", index, record->id,
                   record->name[0] ? record->name : "unknown",
                   record->arg_bytes, record->stub_return_pc,
                   record->return_pc, record->status,
                   pw_unix_call_outcome_name(record->outcome),
                   record->argument_index);
            printf("kind=host-wine-call-args index=%u a0=0x%08x a1=0x%08x "
                   "a2=0x%08x a3=0x%08x a4=0x%08x a5=0x%08x esp=0x%08x\n",
                   index, record->args[0], record->args[1], record->args[2],
                   record->args[3], record->args[4], record->args[5],
                   record->esp);
        }
    }
    printf("kind=host-wine-verdict accepted=%d stop=%s entry_id=0x%08x "
           "syscall=0x%08x stop_call=0x%08x stop_call_args=%#x,%#x,%#x,%#x,%#x,%#x "
           "exit_call=0x%08x exit_status=%#x retired=%llu\n",
           status == PW_OK && pw_wine_stop_is_acceptance(report.stop),
           pw_wine_stop_name(report.stop), report.stub_syscall_id,
           report.observed_syscall_id, report.stop_call_id,
           report.stop_call_args[0], report.stop_call_args[1],
           report.stop_call_args[2], report.stop_call_args[3],
           report.stop_call_args[4], report.stop_call_args[5],
           report.exit_call_id, report.exit_status,
           (unsigned long long)report.retired);
    /* A classified stop is evidence, never a compatibility claim. */
    return status == PW_OK && pw_wine_stop_is_acceptance(report.stop) ? 0 : 1;
}
